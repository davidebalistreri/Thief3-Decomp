// Standalone SceneIR D3D12 consumer. The compatibility draw trace is not its input.
#include "scene_packet.hpp"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

namespace {
using namespace t3render::scene;
template<class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};
struct Handle { HANDLE p = nullptr; ~Handle() { if (p && p != INVALID_HANDLE_VALUE) CloseHandle(p); } };
struct Module { HMODULE p = nullptr; ~Module() { if (p) FreeLibrary(p); } };
struct GpuTexture { ComPtr<ID3D12Resource> texture, upload; };
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R8G8B8A8_UNORM;

bool Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s: HRESULT=0x%08lx\n", operation,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}

bool ReadPacket(const wchar_t* path, Frame& frame) {
    FILE* file = _wfopen(path, L"rb");
    if (!file) { std::fprintf(stderr, "cannot open SceneIR packet\n"); return false; }
    std::vector<std::uint8_t> bytes(kMaxPacketBytes + 1);
    const std::size_t size = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool error = std::ferror(file) != 0;
    const bool close_error = std::fclose(file) != 0;
    if (error || close_error || !ParsePacket(bytes.data(), size, frame)) {
        std::fprintf(stderr, "invalid SceneIR packet\n"); return false;
    }
    return true;
}

using CompileFn = decltype(&D3DCompile);
bool Compile(CompileFn compile, const char* source, const char* profile, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
                               D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                               blob.put(), errors.put());
    if (errors.p) { std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr); std::fputc('\n', stderr); }
    return Check(hr, profile) && blob.p;
}

bool SelectDevice(ComPtr<ID3D12Device>& device) {
    ComPtr<IDXGIFactory4> factory;
    if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1")) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, adapter.put());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (!Check(hr, "EnumAdapters1")) return false;
        DXGI_ADAPTER_DESC1 desc{};
        if (!Check(adapter->GetDesc1(&desc), "GetDesc1")) return false;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())))) return true;
    }
    ComPtr<IDXGIAdapter> warp;
    return Check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter") &&
           Check(D3D12CreateDevice(warp.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "D3D12CreateDevice");
}

D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1; return heap;
}
D3D12_RESOURCE_DESC Buffer(UINT64 size) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return desc;
}
D3D12_RESOURCE_DESC Image(UINT width, UINT height, UINT16 mips, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = mips;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags = flags;
    return desc;
}
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

bool UploadTexture(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Texture& texture, GpuTexture& gpu) {
    const auto desc = Image(texture.mips[0].width, texture.mips[0].height, static_cast<UINT16>(texture.mips.size()),
                            kColor, D3D12_RESOURCE_FLAG_NONE);
    const auto default_heap = Heap(D3D12_HEAP_TYPE_DEFAULT), upload_heap = Heap(D3D12_HEAP_TYPE_UPLOAD);
    if (!Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(gpu.texture.put())), "CreateTexture")) return false;
    const UINT count = static_cast<UINT>(texture.mips.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_sizes(count);
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), row_sizes.data(), &total);
    if (!total || total > SIZE_MAX) return false;
    const auto upload_desc = Buffer(total);
    if (!Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(gpu.upload.put())), "CreateTextureUpload")) return false;
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (!Check(gpu.upload->Map(0, &no_read, &mapped), "MapTextureUpload") || !mapped) return false;
    bool valid = true;
    for (UINT i = 0; i < count; ++i) {
        const auto& mip = texture.mips[i];
        const auto& fp = footprints[i];
        const UINT64 row_bytes = UINT64(mip.width) * 4;
        if (rows[i] != mip.height || row_sizes[i] != row_bytes || fp.Footprint.RowPitch < row_bytes ||
            fp.Offset > total || row_bytes > total - fp.Offset ||
            UINT64(mip.height - 1) > (total - fp.Offset - row_bytes) / fp.Footprint.RowPitch) { valid = false; break; }
        for (UINT row = 0; row < mip.height; ++row)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + fp.Offset + row * fp.Footprint.RowPitch,
                        mip.rgba.data() + row * static_cast<std::size_t>(row_bytes), static_cast<std::size_t>(row_bytes));
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(total)};
    gpu.upload->Unmap(0, &written);
    if (!valid) return false;
    for (UINT i = 0; i < count; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = gpu.texture.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = gpu.upload.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[i];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(list, gpu.texture.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool WriteAll(HANDLE file, const void* data, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size) { DWORD done = 0; if (!WriteFile(file, bytes, size, &done, nullptr) || !done) return false; bytes += done; size -= done; }
    return true;
}
bool WriteImage(const wchar_t* path, const std::uint8_t* bytes, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                UINT width, UINT height) {
    Handle file{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.p == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "cannot create output image: %lu\n", GetLastError()); return false; }
    char header[64];
    const int n = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", width, height);
    if (n <= 0 || !WriteAll(file.p, header, static_cast<DWORD>(n))) return false;
    std::vector<std::uint8_t> rgb(width * 3);
    for (UINT y = 0; y < height; ++y) {
        const auto* row = bytes + fp.Offset + y * fp.Footprint.RowPitch;
        for (UINT x = 0; x < width; ++x) { rgb[x * 3] = row[x * 4]; rgb[x * 3 + 1] = row[x * 4 + 1]; rgb[x * 3 + 2] = row[x * 4 + 2]; }
        if (!WriteAll(file.p, rgb.data(), static_cast<DWORD>(rgb.size()))) return false;
    }
    return true;
}

[[noreturn]] void AbortInFlight(const char* message) {
    std::fprintf(stderr, "%s; GPU resources retained until process exit\n", message);
    std::fflush(nullptr); std::_Exit(4);
}

int Render(const Frame& frame, const wchar_t* output) {
    ComPtr<ID3D12Device> device;
    if (!SelectDevice(device)) return 1;
    Module module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!module.p) { std::fprintf(stderr, "d3dcompiler_47.dll unavailable\n"); return 1; }
    FARPROC address = GetProcAddress(module.p, "D3DCompile");
    CompileFn compile = nullptr;
    static_assert(sizeof(compile) == sizeof(address), "unexpected function pointer size");
    std::memcpy(&compile, &address, sizeof(compile));
    if (!compile) return 1;
    // Keep the varying first so Wine's DXBC-to-SPIR-V translation assigns it
    // the same location as the pixel input; SV_Position is a built-in.
    static const char vs_source[] =
        "cbuffer Camera:register(b0){row_major float4x4 view;row_major float4x4 proj;float2 size;}"
        "struct O{float4 uv:TEXCOORD0;float4 p:SV_Position;};"
        "O main(float3 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=mul(mul(float4(p,1),view),proj);"
        "o.p.x+=o.p.w/size.x;o.p.y-=o.p.w/size.y;o.uv=float4(uv,0,0);return o;}";
    static const char ps_source[] =
        "Texture2D diffuse:register(t0);SamplerState samp:register(s0);"
        "cbuffer Alpha:register(b1){uint mode;float reference;float lod_bias;uint ambient;float4 ambient_color;}"
        "float4 main(float4 uv:TEXCOORD0):SV_Target{float4 c=diffuse.SampleBias(samp,uv.xy,lod_bias);"
        "bool keep=(mode==0)||(mode==2&&c.a<reference)||(mode==3&&c.a==reference)||"
        "(mode==4&&c.a<=reference)||(mode==5&&c.a>reference)||(mode==6&&c.a!=reference)||"
        "(mode==7&&c.a>=reference)||(mode==8);if(!keep)discard;"
        "if(ambient!=0)c.rgb=ambient_color.rgb;return c;}";
    ComPtr<ID3DBlob> vs, ps;
    if (!Compile(compile, vs_source, "vs_5_0", vs) || !Compile(compile, ps_source, "ps_5_0", ps)) return 1;

    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12Fence> fence;
    if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateQueue") ||
        !Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateAllocator") ||
        !Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence")) return 1;
    Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!event.p) return 1;
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 36;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1; params[2].Constants.Num32BitValues = 8;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{}; root_desc.NumParameters = 3; root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1; root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, root_errors; ComPtr<ID3D12RootSignature> root;
    if (!Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, root_blob.put(), root_errors.put()), "SerializeRoot") ||
        !Check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(root.put())), "CreateRoot")) return 1;
    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.p;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {input, 2}; pso.SampleMask = UINT_MAX;
    for (auto& blend : pso.BlendState.RenderTarget) {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE; pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = kColor; pso.DSVFormat = DXGI_FORMAT_D32_FLOAT; pso.SampleDesc.Count = 1;
    const D3D12_BLEND color_factors[] = {D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR,
        D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_COLOR,
        D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};
    const D3D12_BLEND alpha_factors[] = {D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_ALPHA,
        D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA,
        D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};
    const D3D12_BLEND_OP operations[] = {D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_REV_SUBTRACT,
                                         D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX};
    const D3D12_COMPARISON_FUNC comparisons[] = {D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
    const D3D12_CULL_MODE culls[] = {D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT};
    struct Pipeline { DrawStyle state; ComPtr<ID3D12PipelineState> object; };
    std::vector<std::unique_ptr<Pipeline>> pipelines;
    std::vector<std::size_t> draw_pipelines;
    for (const auto& draw : frame.draws) {
        const auto& state = draw.state;
        std::size_t index = 0;
        while (index < pipelines.size() && !pipelines[index]->state.same_pipeline(state)) ++index;
        if (index == pipelines.size()) {
            if (pipelines.size() == 128) { std::fprintf(stderr, "too many pipeline variants\n"); return 1; }
            auto variant = std::make_unique<Pipeline>(); variant->state = state;
            auto description = pso;
            auto& blend = description.BlendState.RenderTarget[0];
            blend.BlendEnable = state.blend_enable != 0;
            blend.SrcBlend = color_factors[state.blend_src]; blend.DestBlend = color_factors[state.blend_dst];
            blend.SrcBlendAlpha = alpha_factors[state.blend_src]; blend.DestBlendAlpha = alpha_factors[state.blend_dst];
            blend.BlendOp = blend.BlendOpAlpha = operations[state.blend_op];
            blend.RenderTargetWriteMask = static_cast<UINT8>(state.color_mask);
            description.RasterizerState.CullMode = culls[state.cull];
            description.DepthStencilState.DepthEnable = state.depth_enable != 0;
            description.DepthStencilState.DepthWriteMask = state.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            description.DepthStencilState.DepthFunc = comparisons[state.depth_compare - 1];
            if (!Check(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(variant->object.put())), "CreatePipeline")) return 1;
            pipelines.push_back(std::move(variant));
        }
        draw_pipelines.push_back(index);
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pipelines[0]->object.p,
                                         IID_PPV_ARGS(list.put())), "CreateList")) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = static_cast<UINT>(frame.textures.size()); heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    if (!Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srv_heap.put())), "CreateSrvHeap")) return 1;
    auto cpu_srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<std::unique_ptr<GpuTexture>> textures;
    for (const auto& texture : frame.textures) {
        auto gpu = std::make_unique<GpuTexture>();
        if (!UploadTexture(device.p, list.p, texture, *gpu)) return 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = kColor; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = static_cast<UINT>(texture.mips.size());
        device->CreateShaderResourceView(gpu->texture.p, &srv, cpu_srv); cpu_srv.ptr += srv_step;
        textures.push_back(std::move(gpu));
    }
    const auto default_heap = Heap(D3D12_HEAP_TYPE_DEFAULT), upload_heap = Heap(D3D12_HEAP_TYPE_UPLOAD), read_heap = Heap(D3D12_HEAP_TYPE_READBACK);
    const auto target_desc = Image(frame.width, frame.height, 1, kColor, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto depth_desc = Image(frame.width, frame.height, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{}; clear.Format = kColor; clear.Color[0] = 22.0f / 255; clear.Color[1] = 24.0f / 255;
    clear.Color[2] = 28.0f / 255; clear.Color[3] = 1;
    D3D12_CLEAR_VALUE depth_clear{}; depth_clear.Format = DXGI_FORMAT_D32_FLOAT; depth_clear.DepthStencil.Depth = 1;
    ComPtr<ID3D12Resource> target, depth;
    if (!Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
             &clear, IID_PPV_ARGS(target.put())), "CreateTarget") ||
        !Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
             &depth_clear, IID_PPV_ARGS(depth.put())), "CreateDepth")) return 1;
    heap_desc = {}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    if (!Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.put())), "CreateRtvHeap")) return 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(dsv_heap.put())), "CreateDsvHeap")) return 1;
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart(); auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.p, nullptr, rtv); device->CreateDepthStencilView(depth.p, nullptr, dsv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 row_size = 0, total = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (rows != frame.height || row_size != UINT64(frame.width) * 4 || fp.Footprint.RowPitch < row_size ||
        !total || total > SIZE_MAX || fp.Offset > total || row_size > total - fp.Offset ||
        UINT64(frame.height - 1) > (total - fp.Offset - row_size) / fp.Footprint.RowPitch) return 1;
    const auto read_desc = Buffer(total);
    ComPtr<ID3D12Resource> readback;
    if (!Check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &read_desc, D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(readback.put())), "CreateReadback")) return 1;
    const std::size_t vertex_bytes = frame.vertices.size() * sizeof(Vertex);
    const auto vertex_desc = Buffer(vertex_bytes);
    ComPtr<ID3D12Resource> vertices;
    if (!Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
              nullptr, IID_PPV_ARGS(vertices.put())), "CreateVertices")) return 1;
    void* mapped = nullptr; const D3D12_RANGE no_read{0, 0};
    if (!Check(vertices->Map(0, &no_read, &mapped), "MapVertices") || !mapped) return 1;
    std::memcpy(mapped, frame.vertices.data(), vertex_bytes);
    const D3D12_RANGE written{0, vertex_bytes}; vertices->Unmap(0, &written);
    list->SetGraphicsRootSignature(root.p);
    ID3D12DescriptorHeap* heaps[] = {srv_heap.p}; list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRoot32BitConstants(0, 16, frame.view, 0);
    list->SetGraphicsRoot32BitConstants(0, 16, frame.projection, 16);
    const float size[] = {static_cast<float>(frame.width), static_cast<float>(frame.height), 0, 0};
    list->SetGraphicsRoot32BitConstants(0, 4, size, 32);
    const D3D12_VIEWPORT viewport{0, 0, size[0], size[1], frame.min_depth, frame.max_depth};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(frame.width), static_cast<LONG>(frame.height)};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv); list->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    const D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(), static_cast<UINT>(vertex_bytes), sizeof(Vertex)};
    list->IASetVertexBuffers(0, 1, &vb); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto gpu_base = srv_heap->GetGPUDescriptorHandleForHeapStart();
    for (std::size_t i = 0; i < frame.draws.size(); ++i) {
        const auto& draw = frame.draws[i];
        auto handle = gpu_base; handle.ptr += UINT64(draw.texture) * srv_step;
        struct AlphaConstants { std::uint32_t mode; float reference, bias; std::uint32_t ambient; float color[4]; } alpha{
            draw.state.alpha_compare, draw.state.alpha_ref / 255.0f, draw.state.mip_lod_bias, draw.ambient_candidate,
            {draw.ambient_color[0], draw.ambient_color[1], draw.ambient_color[2], draw.ambient_color[3]}};
        static_assert(sizeof(AlphaConstants) == 8 * sizeof(std::uint32_t), "unexpected pixel constants layout");
        list->SetPipelineState(pipelines[draw_pipelines[i]]->object.p);
        list->SetGraphicsRootDescriptorTable(1, handle);
        list->SetGraphicsRoot32BitConstants(2, 8, &alpha, 0);
        list->DrawInstanced(draw.vertex_count, 1, draw.first_vertex, 0);
    }
    Transition(list.p, target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.p; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = target.p; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!Check(list->Close(), "CloseList")) return 1;
    ID3D12CommandList* lists[] = {list.p}; queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.p, 1))) AbortInFlight("Queue signal failed");
    UINT64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) AbortInFlight("Device removed");
    if (completed < 1 && (FAILED(fence->SetEventOnCompletion(1, event.p)) || WaitForSingleObject(event.p, 10000) != WAIT_OBJECT_0))
        AbortInFlight("Fence wait failed");
    completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX || completed < 1) AbortInFlight("Fence did not complete");
    if (!Check(device->GetDeviceRemovedReason(), "Device status")) return 1;
    const D3D12_RANGE read{0, static_cast<SIZE_T>(total)}; mapped = nullptr;
    if (!Check(readback->Map(0, &read, &mapped), "MapReadback") || !mapped) return 1;
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    std::size_t changed = 0;
    for (UINT y = 0; y < frame.height; ++y) for (UINT x = 0; x < frame.width; ++x) {
        const auto* pixel = pixels + fp.Offset + y * fp.Footprint.RowPitch + x * 4;
        if (pixel[0] != 22 || pixel[1] != 24 || pixel[2] != 28) ++changed;
    }
    const bool ok = changed && WriteImage(output, pixels, fp, frame.width, frame.height);
    const D3D12_RANGE no_write{0, 0}; readback->Unmap(0, &no_write);
    if (!changed) { std::fprintf(stderr, "no visible pixels differ from clear; no image written\n"); return 1; }
    if (!ok) { std::fprintf(stderr, "image write failed; partial file may remain\n"); return 1; }
    std::printf("sceneir_dx12=ok textures=%zu draws=%zu triangles=%zu changed_pixels=%zu\n",
                frame.textures.size(), frame.draws.size(), frame.vertices.size() / 3, changed);
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || std::wcscmp(argv[2], L"--output") || !argv[3][0]) {
        std::fwprintf(stderr, L"usage: t3render_scene_d3d12 <scene.bin> --output <new-image.ppm>\n"); return 2;
    }
    Frame frame;
    if (!ReadPacket(argv[1], frame)) return 2;
    return Render(frame, argv[3]);
}
