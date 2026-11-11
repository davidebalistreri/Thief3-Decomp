// Standalone D3D12 offscreen probe. It uses no game assets or gameplay state.
// It is not a D3D8 compatibility renderer and performs no DXR work.
#include "d3d12_probe_pixels.hpp"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {
constexpr UINT kWidth = 256;
constexpr UINT kHeight = 256;
constexpr DWORD kFenceTimeoutMs = 10000;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

template <class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Module {
    HMODULE value = nullptr;
    ~Module() { if (value) FreeLibrary(value); }
};

bool Check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s failed HRESULT=0x%08lx\n", what,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}

std::string Utf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "<unavailable>";
    std::vector<char> buffer(static_cast<std::size_t>(length));
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer.data(), length, nullptr, nullptr)) return "<unavailable>";
    return std::string(buffer.data());
}

void PrintBlob(const char* prefix, ID3DBlob* blob) {
    if (!blob) return;
    std::fprintf(stderr, "%s: ", prefix);
    std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), stderr);
    std::fputc('\n', stderr);
}

using CompileFn = decltype(&D3DCompile);
CompileFn LoadCompiler(Module& module) {
    module.value = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module.value) {
        std::fprintf(stderr, "d3dcompiler_47.dll unavailable (Win32 error %lu)\n", GetLastError());
        return nullptr;
    }
    FARPROC address = GetProcAddress(module.value, "D3DCompile");
    CompileFn compile = nullptr;
    static_assert(sizeof(compile) == sizeof(address), "Unexpected Windows function pointer size");
    std::memcpy(&compile, &address, sizeof(compile));
    if (!compile) std::fprintf(stderr, "D3DCompile export unavailable\n");
    return compile;
}

bool CompileShader(CompileFn compile, const char* source, const char* target, ComPtr<ID3DBlob>& shader) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target,
                               D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                               0, shader.put(), errors.put());
    if (errors.value) PrintBlob(target, errors.value);
    return Check(hr, target) && shader.value;
}

bool SelectDevice(IDXGIFactory4* factory, bool request_warp, ComPtr<IDXGIAdapter1>& selected,
                  ComPtr<ID3D12Device>& device) {
    if (!request_warp) {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT enumerated = factory->EnumAdapters1(index, adapter.put());
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (!Check(enumerated, "EnumAdapters1")) return false;
            DXGI_ADAPTER_DESC1 info{};
            if (!Check(adapter->GetDesc1(&info), "GetDesc1")) return false;
            const std::string name = Utf8(info.Description);
            if (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                std::fprintf(stderr, "Skipping software adapter=%s vendor=0x%04lx device=0x%04lx\n",
                             name.c_str(), static_cast<unsigned long>(info.VendorId),
                             static_cast<unsigned long>(info.DeviceId));
                continue;
            }
            ID3D12Device* candidate = nullptr;
            const HRESULT hr = D3D12CreateDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&candidate));
            if (FAILED(hr) || !candidate) {
                std::fprintf(stderr,
                             "D3D12CreateDevice rejected adapter=%s vendor=0x%04lx device=0x%04lx "
                             "HRESULT=0x%08lx%s\n",
                             name.c_str(), static_cast<unsigned long>(info.VendorId),
                             static_cast<unsigned long>(info.DeviceId),
                             static_cast<unsigned long>(static_cast<std::uint32_t>(hr)),
                             !candidate && SUCCEEDED(hr) ? " (null device)" : "");
                if (candidate) candidate->Release();
                continue;
            }
            device.value = candidate;
            selected.value = adapter.value;
            adapter.value = nullptr;
            return true;
        }
        std::fprintf(stderr, "No hardware adapter created a D3D12 device; trying WARP\n");
    }
    ComPtr<IDXGIAdapter> warp;
    if (!Check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter")) return false;
    if (!Check(warp->QueryInterface(IID_PPV_ARGS(selected.put())), "WARP QueryInterface")) return false;
    return Check(D3D12CreateDevice(selected.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())),
                 "D3D12CreateDevice(WARP)");
}

void ReportCapabilities(IDXGIAdapter1* adapter, ID3D12Device* device) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        const std::string name = Utf8(desc.Description);
        std::printf("adapter=%s vendor=0x%04lx device=0x%04lx software=%s\n", name.c_str(),
                    static_cast<unsigned long>(desc.VendorId), static_cast<unsigned long>(desc.DeviceId),
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "yes (WARP or software)" : "no");
    } else {
        std::printf("adapter=<unknown> software=<unknown>\n");
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS features{};
    const UINT num_levels = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
    HRESULT level_hr = E_FAIL;
    for (UINT first = 0; first < num_levels; ++first) {
        features.NumFeatureLevels = num_levels - first;
        features.pFeatureLevelsRequested = levels + first;
        level_hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &features, sizeof(features));
        if (SUCCEEDED(level_hr)) break;
    }
    if (SUCCEEDED(level_hr)) std::printf("feature_level=0x%04x\n", static_cast<unsigned>(features.MaxSupportedFeatureLevel));
    else std::printf("feature_level=<unknown> HRESULT=0x%08lx\n", static_cast<unsigned long>(static_cast<std::uint32_t>(level_hr)));
    const D3D_SHADER_MODEL models[] = {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
    D3D12_FEATURE_DATA_SHADER_MODEL shader{};
    HRESULT shader_hr = E_FAIL;
    for (D3D_SHADER_MODEL model : models) {
        shader.HighestShaderModel = model;
        shader_hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader, sizeof(shader));
        if (SUCCEEDED(shader_hr)) break;
    }
    if (SUCCEEDED(shader_hr)) std::printf("device_shader_model=0x%02x (query capped at 6.7)\n", static_cast<unsigned>(shader.HighestShaderModel));
    else std::printf("device_shader_model=<unknown> HRESULT=0x%08lx\n", static_cast<unsigned long>(static_cast<std::uint32_t>(shader_hr)));
    std::printf("shader_compiler=D3DCompile (SM 5.0 DXBC); DXR=not_tested\n");
}

bool WriteAll(HANDLE file, const void* data, DWORD size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, p, size, &written, nullptr)) return false;
        if (!written) { SetLastError(ERROR_WRITE_FAULT); return false; }
        p += written;
        size -= written;
    }
    return true;
}

bool WritePpm(const wchar_t* path, const std::uint8_t* data, std::size_t offset, std::size_t pitch) {
    Handle file{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "cannot create output image (error %lu; existing files are not replaced)\n", GetLastError());
        return false;
    }
    static const char header[] = "P6\n256 256\n255\n";
    auto failed_write = [&] {
        const DWORD error = GetLastError();
        const std::string name = Utf8(path);
        std::fprintf(stderr, "incomplete output remains at %s (Win32 error %lu)\n", name.c_str(), error);
        return false;
    };
    if (!WriteAll(file.value, header, static_cast<DWORD>(sizeof(header) - 1))) return failed_write();
    std::vector<std::uint8_t> row(kWidth * 3);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const auto* p = data + offset + y * pitch + x * 4;
            row[x * 3] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2];
        }
        if (!WriteAll(file.value, row.data(), static_cast<DWORD>(row.size()))) return failed_write();
    }
    return true;
}

// Once work has been submitted, failing or timing out means it may still be
// using our resources. Let process termination reclaim them instead of running
// destructors against an unconfirmed GPU queue.
[[noreturn]] void AbortInFlight(ID3D12Device* device, const char* reason) {
    std::fprintf(stderr, "%s; device_removed_reason=0x%08lx\n", reason,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(device->GetDeviceRemovedReason())));
    std::fflush(nullptr);
    std::_Exit(4);
}

int Run(bool request_warp, const wchar_t* output) {
    ComPtr<IDXGIFactory4> factory;
    if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1")) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (!SelectDevice(factory.value, request_warp, adapter, device)) return 1;
    ReportCapabilities(adapter.value, device.value);

    // Keep the DLL loaded until shader blobs have been released.
    Module compiler_module;
    CompileFn compile = LoadCompiler(compiler_module);
    if (!compile) return 1;
    static const char vertex_source[] =
        "float4 main(uint i : SV_VertexID) : SV_Position {"
        "  float2 p[3] = { float2(-0.8, -0.7), float2(0.0, 0.8), float2(0.8, -0.7) };"
        "  return float4(p[i], 0.0, 1.0); }";
    static const char pixel_source[] = "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";
    ComPtr<ID3DBlob> vertex_shader, pixel_shader;
    if (!CompileShader(compile, vertex_source, "vs_5_0", vertex_shader) ||
        !CompileShader(compile, pixel_source, "ps_5_0", pixel_shader)) return 1;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> fence;
    if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateCommandQueue") ||
        !Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())),
               "CreateCommandAllocator") ||
        !Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence")) return 1;
    Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!event.value) { std::fprintf(stderr, "CreateEventW failed error=%lu\n", GetLastError()); return 1; }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (!Check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.put())), "CreateDescriptorHeap")) return 1;
    D3D12_HEAP_PROPERTIES target_heap{};
    target_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    target_heap.CreationNodeMask = 1; target_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = kWidth; target_desc.Height = kHeight;
    target_desc.DepthOrArraySize = 1; target_desc.MipLevels = 1; target_desc.Format = kFormat;
    target_desc.SampleDesc.Count = 1; target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{}; clear.Format = kFormat; clear.Color[3] = 1.0f;
    ComPtr<ID3D12Resource> target;
    if (!Check(device->CreateCommittedResource(&target_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                IID_PPV_ARGS(target.put())), "CreateCommittedResource(render target)")) return 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.value, nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 row_size = 0, total_size = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, &rows, &row_size, &total_size);
    if (rows != kHeight || row_size != kWidth * 4 || footprint.Footprint.RowPitch < row_size ||
        !total_size || total_size > static_cast<UINT64>(SIZE_MAX)) {
        std::fprintf(stderr, "unsupported readback footprint\n"); return 1;
    }
    D3D12_HEAP_PROPERTIES read_heap{}; read_heap.Type = D3D12_HEAP_TYPE_READBACK;
    read_heap.CreationNodeMask = 1; read_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC read_desc{};
    read_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; read_desc.Width = total_size;
    read_desc.Height = 1; read_desc.DepthOrArraySize = 1; read_desc.MipLevels = 1;
    read_desc.SampleDesc.Count = 1; read_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (!Check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &read_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(readback.put())), "CreateCommittedResource(readback)")) return 1;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, root_errors;
    const HRESULT root_hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                        root_blob.put(), root_errors.put());
    if (root_errors.value) PrintBlob("root signature", root_errors.value);
    if (!Check(root_hr, "D3D12SerializeRootSignature")) return 1;
    ComPtr<ID3D12RootSignature> root;
    if (!Check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                          IID_PPV_ARGS(root.put())), "CreateRootSignature")) return 1;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.value;
    pso.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    pso.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    for (auto& blend : pso.BlendState.RenderTarget) {
        blend.SrcBlend = D3D12_BLEND_ONE; blend.DestBlend = D3D12_BLEND_ZERO; blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE; pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pso.DepthStencilState.DepthEnable = FALSE; pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                      D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = kFormat; pso.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(pipeline.put())), "CreateGraphicsPipelineState")) return 1;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.value, pipeline.value,
                                         IID_PPV_ARGS(list.put())), "CreateCommandList")) return 1;
    list->SetGraphicsRootSignature(root.value);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.value;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.value; destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target.value; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!Check(list->Close(), "Close command list")) return 1;
    ID3D12CommandList* lists[] = {list.value};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.value, 1))) AbortInFlight(device.value, "Queue::Signal failed");
    UINT64 completed = fence->GetCompletedValue();
    if (d3d12_probe::EvaluateFence(completed, 1) == d3d12_probe::FenceState::DeviceRemoved)
        AbortInFlight(device.value, "Device removed before fence completion");
    if (d3d12_probe::EvaluateFence(completed, 1) == d3d12_probe::FenceState::Waiting) {
        if (FAILED(fence->SetEventOnCompletion(1, event.value))) AbortInFlight(device.value, "Fence event failed");
        if (WaitForSingleObject(event.value, kFenceTimeoutMs) != WAIT_OBJECT_0) AbortInFlight(device.value, "Fence wait failed or timed out");
    }
    completed = fence->GetCompletedValue();
    const auto fence_state = d3d12_probe::EvaluateFence(completed, 1);
    if (fence_state == d3d12_probe::FenceState::DeviceRemoved) AbortInFlight(device.value, "Device removed during fence wait");
    if (fence_state != d3d12_probe::FenceState::Complete) AbortInFlight(device.value, "Fence was signalled without completion");
    if (!Check(device->GetDeviceRemovedReason(), "device status after fence")) return 1;
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    if (!Check(readback->Map(0, &read_range, &mapped), "readback Map") || !mapped) return 1;
    d3d12_probe::PixelStats stats;
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    const bool pixel_ok = d3d12_probe::AnalyzeTriangle256(pixels, static_cast<std::size_t>(total_size),
        static_cast<std::size_t>(footprint.Offset), footprint.Footprint.RowPitch, stats);
    const bool image_ok = !output || (pixel_ok && WritePpm(output, pixels, static_cast<std::size_t>(footprint.Offset),
                                                          footprint.Footprint.RowPitch));
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    if (!pixel_ok) { std::fprintf(stderr, "pixel checks or readback layout failed\n"); return 1; }
    if (!image_ok) return 1;
    std::printf("readback=%ux%u pitch=%u red_pixels=%zu fnv1a=%016llx\n", kWidth, kHeight,
                footprint.Footprint.RowPitch, stats.red_pixels, static_cast<unsigned long long>(stats.fnv1a));
    std::printf("result=PASS; hash is only comparable on the same adapter and driver\n");
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool warp = false;
    const wchar_t* output = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--warp")) warp = true;
        else if (!std::wcscmp(argv[i], L"--output") && i + 1 < argc) output = argv[++i];
        else { std::fwprintf(stderr, L"usage: d3d12_offscreen [--warp] [--output new-image.ppm]\n"); return 2; }
    }
    return Run(warp, output);
}
