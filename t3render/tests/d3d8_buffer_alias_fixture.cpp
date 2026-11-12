// Isolated D3D8 alias/WRITEONLY-buffer experiment. Never launches TDS.
// Reuse the existing fixture's COM helpers and PPM readback, not its main.
#define wmain unused_stateblock_fixture_wmain
#include "d3d8_stateblock_buffer_fixture.cpp"
#undef wmain

namespace {
const char* JsonBool(bool value) { return value ? "true" : "false"; }
unsigned long Hr(HRESULT value) { return static_cast<unsigned long>(static_cast<std::uint32_t>(value)); }

void Operation(const char* name, HRESULT hr) {
    std::printf("alias-json {\"event\":\"call\",\"name\":\"%s\",\"hr\":\"0x%08lx\"}\n", name, Hr(hr));
}

bool Named(const wchar_t* selected, const wchar_t* value) { return std::wcscmp(selected, value) == 0; }

void* Vtable(const void* value) {
    void* table = nullptr;
    if (value) std::memcpy(&table, value, sizeof(table));
    return table;
}

struct Alias {
    ComPtr<IDirect3DDevice8> device;
    HRESULT hr = E_FAIL;
    const char* stage = "unselected";
};

// Each successful GetDevice/GetContainer returns its own COM reference. The
// owning resources can be released after acquisition; the device stays pinned.
Alias* Acquire(const wchar_t* selected, IDirect3DDevice8* device, Pair& pair, Alias& alias) {
    if (Named(selected, L"device_qi")) {
        alias.stage = "device_qi";
        alias.hr = device->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(alias.device.put()));
    } else if (Named(selected, L"vb_device") || Named(selected, L"ib_device")) {
        alias.stage = "buffer_get_device";
        alias.hr = Named(selected, L"vb_device") ? pair.vertex->GetDevice(alias.device.put())
                                                    : pair.index->GetDevice(alias.device.put());
    } else if (Named(selected, L"vb_unknown") || Named(selected, L"ib_unknown")) {
        ComPtr<IUnknown> unknown;
        alias.stage = "buffer_qi_unknown";
        alias.hr = Named(selected, L"vb_unknown")
            ? pair.vertex->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(unknown.put()))
            : pair.index->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(unknown.put()));
        if (FAILED(alias.hr) || !unknown.value) return &alias;
        alias.stage = "buffer_qi_roundtrip";
        if (Named(selected, L"vb_unknown")) {
            ComPtr<IDirect3DVertexBuffer8> round;
            alias.hr = unknown->QueryInterface(IID_IDirect3DVertexBuffer8, reinterpret_cast<void**>(round.put()));
            if (FAILED(alias.hr) || !round.value) return &alias;
            if (!SameIdentity(pair.vertex.value, round.value)) { alias.hr = E_FAIL; return &alias; }
            alias.stage = "buffer_roundtrip_get_device";
            alias.hr = round->GetDevice(alias.device.put());
        } else {
            ComPtr<IDirect3DIndexBuffer8> round;
            alias.hr = unknown->QueryInterface(IID_IDirect3DIndexBuffer8, reinterpret_cast<void**>(round.put()));
            if (FAILED(alias.hr) || !round.value) return &alias;
            if (!SameIdentity(pair.index.value, round.value)) { alias.hr = E_FAIL; return &alias; }
            alias.stage = "buffer_roundtrip_get_device";
            alias.hr = round->GetDevice(alias.device.put());
        }
    } else if (Named(selected, L"texture_device") || Named(selected, L"texture_resource") ||
               Named(selected, L"texture_base") || Named(selected, L"texture_bound") ||
               Named(selected, L"surface_device") || Named(selected, L"surface_container_device") ||
               Named(selected, L"surface_container_unknown") || Named(selected, L"surface_container_texture")) {
        ComPtr<IDirect3DTexture8> texture;
        alias.stage = "create_texture";
        alias.hr = device->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put());
        if (FAILED(alias.hr) || !texture.value) return &alias;
        if (Named(selected, L"texture_device")) {
            alias.stage = "texture_get_device"; alias.hr = texture->GetDevice(alias.device.put());
        } else if (Named(selected, L"texture_resource")) {
            ComPtr<IDirect3DResource8> resource;
            alias.stage = "texture_qi_resource";
            alias.hr = texture->QueryInterface(IID_IDirect3DResource8, reinterpret_cast<void**>(resource.put()));
            if (FAILED(alias.hr) || !resource.value) return &alias;
            alias.stage = "texture_resource_get_device"; alias.hr = resource->GetDevice(alias.device.put());
        } else if (Named(selected, L"texture_base")) {
            ComPtr<IDirect3DBaseTexture8> base;
            alias.stage = "texture_qi_base";
            alias.hr = texture->QueryInterface(IID_IDirect3DBaseTexture8, reinterpret_cast<void**>(base.put()));
            if (FAILED(alias.hr) || !base.value) return &alias;
            alias.stage = "texture_base_get_device"; alias.hr = base->GetDevice(alias.device.put());
        } else if (Named(selected, L"texture_bound")) {
            alias.stage = "bind_texture"; alias.hr = device->SetTexture(0, texture.value);
            if (FAILED(alias.hr)) return &alias;
            ComPtr<IDirect3DBaseTexture8> bound;
            alias.stage = "get_texture"; alias.hr = device->GetTexture(0, bound.put());
            if (SUCCEEDED(alias.hr) && bound.value) {
                alias.stage = "bound_texture_get_device"; alias.hr = bound->GetDevice(alias.device.put());
            }
            const HRESULT unset = device->SetTexture(0, nullptr);
            if (FAILED(unset)) { alias.device.reset(); alias.stage = "unbind_texture"; alias.hr = unset; }
        } else {
            ComPtr<IDirect3DSurface8> surface;
            alias.stage = "texture_surface"; alias.hr = texture->GetSurfaceLevel(0, surface.put());
            if (FAILED(alias.hr) || !surface.value) return &alias;
            if (Named(selected, L"surface_device")) {
                alias.stage = "surface_get_device"; alias.hr = surface->GetDevice(alias.device.put());
            } else if (Named(selected, L"surface_container_device")) {
                alias.stage = "surface_container_device";
                alias.hr = surface->GetContainer(IID_IDirect3DDevice8, reinterpret_cast<void**>(alias.device.put()));
            } else if (Named(selected, L"surface_container_texture")) {
                ComPtr<IDirect3DTexture8> parent;
                alias.stage = "surface_container_texture";
                alias.hr = surface->GetContainer(IID_IDirect3DTexture8, reinterpret_cast<void**>(parent.put()));
                if (FAILED(alias.hr) || !parent.value) return &alias;
                if (!SameIdentity(texture.value, parent.value)) { alias.hr = E_FAIL; return &alias; }
                alias.stage = "container_texture_get_device"; alias.hr = parent->GetDevice(alias.device.put());
            } else {
                ComPtr<IUnknown> container;
                alias.stage = "surface_container_unknown";
                alias.hr = surface->GetContainer(IID_IUnknown, reinterpret_cast<void**>(container.put()));
                if (FAILED(alias.hr) || !container.value) return &alias;
                alias.stage = "container_unknown_qi_device";
                alias.hr = container->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(alias.device.put()));
            }
        }
    } else if (Named(selected, L"backbuffer_device") || Named(selected, L"backbuffer_container_device")) {
        ComPtr<IDirect3DSurface8> back;
        alias.stage = "get_backbuffer"; alias.hr = device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put());
        if (FAILED(alias.hr) || !back.value) return &alias;
        if (Named(selected, L"backbuffer_device")) {
            alias.stage = "backbuffer_get_device"; alias.hr = back->GetDevice(alias.device.put());
        } else {
            alias.stage = "backbuffer_container_device";
            alias.hr = back->GetContainer(IID_IDirect3DDevice8, reinterpret_cast<void**>(alias.device.put()));
        }
    } else if (Named(selected, L"cube_device") || Named(selected, L"cube_face_device")) {
        ComPtr<IDirect3DCubeTexture8> cube;
        alias.stage = "create_cube";
        alias.hr = device->CreateCubeTexture(32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put());
        if (FAILED(alias.hr) || !cube.value) return &alias;
        if (Named(selected, L"cube_device")) {
            alias.stage = "cube_get_device"; alias.hr = cube->GetDevice(alias.device.put());
        } else {
            ComPtr<IDirect3DSurface8> face;
            alias.stage = "cube_face"; alias.hr = cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, face.put());
            if (FAILED(alias.hr) || !face.value) return &alias;
            alias.stage = "cube_face_get_device"; alias.hr = face->GetDevice(alias.device.put());
        }
    } else if (Named(selected, L"volume_texture_device") || Named(selected, L"volume_device")) {
        ComPtr<IDirect3DVolumeTexture8> texture;
        alias.stage = "create_volume_texture";
        alias.hr = device->CreateVolumeTexture(16, 16, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put());
        if (FAILED(alias.hr) || !texture.value) return &alias;
        if (Named(selected, L"volume_texture_device")) {
            alias.stage = "volume_texture_get_device"; alias.hr = texture->GetDevice(alias.device.put());
        } else {
            ComPtr<IDirect3DVolume8> volume;
            alias.stage = "volume_level"; alias.hr = texture->GetVolumeLevel(0, volume.put());
            if (FAILED(alias.hr) || !volume.value) return &alias;
            alias.stage = "volume_get_device"; alias.hr = volume->GetDevice(alias.device.put());
        }
    } else if (Named(selected, L"swap_surface_device")) {
        D3DPRESENT_PARAMETERS pp{};
        D3DDISPLAYMODE display{};
        alias.stage = "swap_display"; alias.hr = device->GetDisplayMode(&display);
        if (FAILED(alias.hr)) return &alias;
        pp.BackBufferWidth = pp.BackBufferHeight = kWidth; pp.BackBufferFormat = display.Format;
        pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE;
        ComPtr<IDirect3DSwapChain8> swap;
        alias.stage = "create_swapchain"; alias.hr = device->CreateAdditionalSwapChain(&pp, swap.put());
        if (FAILED(alias.hr) || !swap.value) return &alias;
        ComPtr<IDirect3DSurface8> back;
        alias.stage = "swap_backbuffer"; alias.hr = swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put());
        if (FAILED(alias.hr) || !back.value) return &alias;
        alias.stage = "swap_surface_get_device"; alias.hr = back->GetDevice(alias.device.put());
    } else return nullptr;
    return &alias;
}

struct InputVertex { float x, y, z; D3DCOLOR color; };

bool Run(HWND window, const wchar_t* selected, const wchar_t* output) {
    ComPtr<IDirect3D8> factory;
    factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    D3DDISPLAYMODE display{};
    if (!factory.value || !Check(factory->GetAdapterDisplayMode(0, &display), "display")) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.Windowed = TRUE; pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Check(factory->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                     &pp, device.put()), "create_device") || !device.value) return false;
    void* const control_vtable = Vtable(device.value);
    if (!control_vtable) return false;
    Pair pair;
    if (!CreatePair(device.value, pair, 0xffff0000, false) || Vtable(device.value) != control_vtable) return false;
    if (!Check(device->SetStreamSource(0, pair.vertex.value, sizeof(Vertex)), "initial_stream") ||
        Vtable(device.value) != control_vtable ||
        !Check(device->SetIndices(pair.index.value, 0), "initial_indices") ||
        !Check(device->SetVertexShader(kFvf), "fvf") || !Check(device->SetPixelShader(0), "pixel") ||
        !Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "lighting") ||
        !Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "cull") ||
        !Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "depth") ||
        !Check(device->SetTexture(0, nullptr), "texture") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "colorop") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "colorarg") ||
        !Check(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "stage1") ||
        !Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0), "clear") ||
        !Check(device->BeginScene(), "begin_scene") ||
        !Check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3, 0, 1), "control_draw")) return false;

    // The captured control draw must have used this exact vtable. A same-
    // pointer alias is not sufficient if another layer replaces it later.
    Alias alias;
    if (!Acquire(selected, device.value, pair, alias)) return false;
    const bool acquired = SUCCEEDED(alias.hr) && alias.device.value;
    std::printf("alias-json {\"event\":\"acquire\",\"stage\":\"%s\",\"hr\":\"0x%08lx\",\"nonnull\":%s}\n",
                alias.stage, Hr(alias.hr), JsonBool(alias.device.value != nullptr));
    if (SUCCEEDED(alias.hr) && !alias.device.value) return false;
    const char* status = "acquisition_failed";
    if (acquired) {
        ComPtr<IUnknown> original, recovered;
        if (!Check(device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(original.put())), "device_identity") ||
            !Check(alias.device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(recovered.put())), "alias_identity") ||
            !original.value || !recovered.value) return false;
        ComPtr<IDirect3DDevice8> round;
        const HRESULT round_hr = recovered->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(round.put()));
        const bool same = alias.device.value == device.value;
        const bool same_unknown = original.value == recovered.value;
        const bool same_vtable = Vtable(alias.device.value) == Vtable(device.value);
        const bool vtable_unchanged = control_vtable && Vtable(device.value) == control_vtable;
        const bool round_same = SUCCEEDED(round_hr) && round.value == device.value;
        std::printf("alias-json {\"event\":\"alias\",\"same_interface\":%s,\"same_iunknown\":%s,"
                    "\"same_vtable\":%s,\"vtable_unchanged\":%s,\"roundtrip_hr\":\"0x%08lx\",\"roundtrip_same\":%s}\n",
                    JsonBool(same), JsonBool(same_unknown), JsonBool(same_vtable), JsonBool(vtable_unchanged),
                    Hr(round_hr), JsonBool(round_same));
        if (!same || !same_unknown || !same_vtable || !vtable_unchanged || !round_same) {
            // Never send a wrapper through an alias whose interception is not established.
            status = "unsafe_alias";
        } else {
            status = "exercised";
            auto dispatch_ok = [&] {
                const bool ok = alias.device.value == device.value && Vtable(device.value) == control_vtable;
                if (!ok) std::fprintf(stderr, "device vtable changed before an alias call\n");
                return ok;
            };
            if (!dispatch_ok()) return false;
            HRESULT hr = alias.device->SetStreamSource(0, pair.vertex.value, sizeof(Vertex)); Operation("set_stream", hr);
            if (FAILED(hr)) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->SetIndices(pair.index.value, 0); Operation("set_indices", hr);
            if (FAILED(hr)) return false;
            ComPtr<IDirect3DVertexBuffer8> got_v;
            ComPtr<IDirect3DIndexBuffer8> got_i;
            UINT stride = 0, base = 0;
            if (!dispatch_ok()) return false;
            hr = alias.device->GetStreamSource(0, got_v.put(), &stride); Operation("get_stream", hr);
            if (FAILED(hr) || !got_v.value || stride != sizeof(Vertex) || !SameIdentity(got_v.value, pair.vertex.value)) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->GetIndices(got_i.put(), &base); Operation("get_indices", hr);
            if (FAILED(hr) || !got_i.value || base != 0 || !SameIdentity(got_i.value, pair.index.value)) return false;
            std::printf("alias-json {\"event\":\"binding_identity\",\"vertex_same\":true,\"index_same\":true}\n");

            // Software vertex processing is optional on a runtime. Its
            // HRESULT is recorded and compared, never promoted to success.
            const InputVertex inputs[] = {{-.5f,-.5f,.5f,0xffff0000}, {0,.5f,.5f,0xffff0000}, {.5f,-.5f,.5f,0xffff0000}};
            ComPtr<IDirect3DVertexBuffer8> input, target;
            hr = device->CreateVertexBuffer(sizeof(inputs), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE,
                                            D3DPOOL_MANAGED, input.put()); Operation("process_input", hr);
            if (FAILED(hr) || !input.value) return false;
            BYTE* mapped = nullptr;
            if (!Check(input->Lock(0, sizeof(inputs), &mapped, 0), "process_lock") || !mapped) return false;
            std::memcpy(mapped, inputs, sizeof(inputs));
            if (!Check(input->Unlock(), "process_unlock")) return false;
            hr = device->CreateVertexBuffer(3 * sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf,
                                            D3DPOOL_SYSTEMMEM, target.put()); Operation("process_target", hr);
            if (FAILED(hr) || !target.value) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE); Operation("process_fvf", hr);
            if (FAILED(hr)) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->SetStreamSource(0, input.value, sizeof(InputVertex)); Operation("process_stream", hr);
            if (FAILED(hr)) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->ProcessVertices(0, 0, 3, target.value, 0); Operation("process_vertices", hr);
            if (!dispatch_ok()) return false;
            hr = alias.device->SetVertexShader(kFvf); Operation("restore_fvf", hr);
            if (FAILED(hr)) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->SetStreamSource(0, pair.vertex.value, sizeof(Vertex)); Operation("restore_stream", hr);
            if (FAILED(hr)) return false;
            // Remove the control triangle so the final image must be drawn by
            // the alias path rather than merely preserving the first image.
            if (!dispatch_ok()) return false;
            hr = alias.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0); Operation("clear_alias", hr);
            if (FAILED(hr)) return false;
            if (!dispatch_ok()) return false;
            hr = alias.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3, 0, 1); Operation("alias_draw", hr);
            if (FAILED(hr)) return false;
        }
    }
    if (!Check(device->EndScene(), "end_scene")) return false;
    std::vector<BYTE> ppm;
    if (!ReadPpm(device.value, ppm) || !WriteNew(output, ppm) ||
        !Check(device->Present(nullptr, nullptr, nullptr, nullptr), "present")) return false;
    std::printf("alias-json {\"event\":\"end\",\"status\":\"%s\"}\n", status);
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 || std::wcscmp(argv[1], L"--case") || !argv[2][0] ||
        std::wcscmp(argv[3], L"--output") || !argv[4][0]) return 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderBufferAliasFixture";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                256, 256, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    const bool ok = Run(window, argv[2], argv[4]);
    DestroyWindow(window); UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
