#include "scene_live_dx12.hpp"
#include "scene_d3d12_common.hpp"
#include "scene_live_lighting.hpp"
#include "scene_live_pbr_shaders.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace t3render::live {
namespace {
using namespace t3render::dx12;
using scene::Frame;
using scene::Vertex;
static_assert(sizeof(Vec3)==sizeof(float)*3,"unexpected position layout");
constexpr DXGI_FORMAT kDepth = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kHdr = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DWORD kFenceTimeoutMs = 10000;
constexpr std::size_t kMaxMaterialGpuBytes = 128u*1024u*1024u;
struct PackedVertex { Vec3 position; Vec2 uv; Vec3 normal; };
static_assert(sizeof(PackedVertex)==sizeof(float)*8,"unexpected resident vertex layout");
struct LightingConstants {
    std::array<float,4> camera_scale{}, options{};
    std::array<std::array<float,4>,kMaxRenderLights> position_range{}, color_intensity{}, direction_outer{}, params{};
};

template<class T> void Release(ComPtr<T>& value) {
    if (value.p) { value.p->Release(); value.p = nullptr; }
}
bool Valid(const Frame& f) {
    if (!f.width || !f.height || f.width > 2048 || f.height > 2048 ||
        !std::isfinite(f.min_depth) || !std::isfinite(f.max_depth) ||
        f.min_depth < 0 || f.max_depth > 1 || f.min_depth > f.max_depth ||
        f.vertices.size() > scene::kMaxTriangles * 3 || f.draws.size() > scene::kMaxDraws) return false;
    for (float x : f.view) if (!std::isfinite(x)) return false;
    for (float x : f.projection) if (!std::isfinite(x)) return false;
    for (const auto& vertex : f.vertices)
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) return false;
    for (const auto& draw : f.draws) {
        if (draw.shading != 1 || draw.state.cull != 0 || draw.state.depth_enable != 1 ||
            draw.state.depth_write != 1 || draw.state.depth_compare != 2 || draw.state.blend_enable ||
            !draw.vertex_count || draw.vertex_count % 3 ||
            std::uint64_t{draw.first_vertex} + draw.vertex_count > f.vertices.size()) return false;
        for (float x : draw.ambient_color) if (!std::isfinite(x) || x < 0 || x > 1) return false;
    }
    return true;
}
bool ValidMaterials(const BedroomMaterialLibrary& library) {
    static constexpr const char* ids[]{"remaster:material:stone_wall_v2", "remaster:material:wood_floor_v2"};
    std::uint32_t expected_size=0;
    for (unsigned i=0;i<2;++i) {
        const auto& material=library.materials[i];
        if (material.id!=ids[i]) return false;
        for (unsigned channel=0;channel<4;++channel) {
            const auto& hash=material.source_sha256[channel];const auto& mips=material.channels[channel].mips;
            if (hash.size()!=64 || !std::all_of(hash.begin(),hash.end(),[](unsigned char c){
                    return (c>='0' && c<='9') || (c>='a' && c<='f');}) || mips.empty()) return false;
            auto width=mips[0].width;
            if (!width || width>1024 || mips[0].height!=width || (width&(width-1))) return false;
            if (!expected_size) expected_size=width;
            if (width!=expected_size) return false;
            std::uint32_t levels=1;for (auto size=width;size>1;size>>=1) ++levels;
            if (mips.size()!=levels) return false;
            for (const auto& mip:mips) {
                if (mip.width!=width || mip.height!=width || mip.rgba.size()!=std::size_t(width)*width*4) return false;
                if (channel==0) for (std::size_t n=3;n<mip.rgba.size();n+=4) if (mip.rgba[n]!=255) return false;
                width=std::max(1u,width/2);
            }
        }
    }
    return true;
}
} // namespace

struct FlatD3D12::Impl {
    struct CachedGeometry {
        std::shared_ptr<const Resource> source;
        ComPtr<ID3D12Resource> vertices, indices;
        std::size_t bytes=0;
        std::uint64_t last_use=0;
        Vec3 low{}, high{};
        bool has_normals=false;
    };
    HWND window = nullptr;
    // Loaded only when the explicitly requested consumer is created. Keep the
    // modules alive until after all COM objects have been released.
    Module dxgi_module, d3d12_module;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    Handle event;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline, texture_pipeline, hdr_flat_pipeline, hdr_texture_pipeline;
    ComPtr<ID3D12PipelineState> pbr_pipeline, pbr_texture_pipeline, pbr_fixture_pipeline, tone_pipeline;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, albedo_heap;
    std::shared_ptr<const BedroomMaterialLibrary> materials;
    std::array<std::array<GpuTexture,4>,2> material_textures;
    UINT albedo_descriptor_size=0;
    ComPtr<ID3D12Resource> target, hdr, depth, vertices, readback, backbuffer, lighting_buffer;
    ComPtr<IDXGISwapChain3> swapchain;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readback_bytes = 0, fence_value = 0;
    std::size_t vertex_capacity = 0;
    std::size_t material_bytes = 0;
    std::size_t cache_budget=0, cache_bytes=0;
    std::uint64_t cache_map=0, cache_device=0, cache_continuity=0, cache_serial=0, uploads=0, evictions=0;
    std::unordered_map<std::string,std::unique_ptr<CachedGeometry>> cache;
    UINT width = 0, height = 0;
    std::uint32_t textured_draws=0,pbr_draws=0;
    bool failed = false, pending = false, prepared = false, prepared_readback = false, lit_commands = false;
    bool pbr_materials_ready = false;

    bool Initialize() {
        dxgi_module.p = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        d3d12_module.p = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dxgi_module.p || !d3d12_module.p) return false;
        FARPROC address = GetProcAddress(dxgi_module.p, "CreateDXGIFactory1");
        decltype(&CreateDXGIFactory1) make_factory = nullptr;
        static_assert(sizeof(make_factory) == sizeof(address), "unexpected function pointer size");
        std::memcpy(&make_factory, &address, sizeof(make_factory));
        address = GetProcAddress(d3d12_module.p, "D3D12CreateDevice");
        decltype(&D3D12CreateDevice) make_device = nullptr;
        std::memcpy(&make_device, &address, sizeof(make_device));
        address = GetProcAddress(d3d12_module.p, "D3D12SerializeRootSignature");
        decltype(&D3D12SerializeRootSignature) serialize_root = nullptr;
        std::memcpy(&serialize_root, &address, sizeof(serialize_root));
        if (!make_factory || !make_device || !serialize_root ||
            !Check(make_factory(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1")) return false;
        for (UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr=factory->EnumAdapters1(i, adapter.put());
            if (hr==DXGI_ERROR_NOT_FOUND) break;
            if (!Check(hr, "EnumAdapters1")) return false;
            DXGI_ADAPTER_DESC1 info{};
            if (!Check(adapter->GetDesc1(&info), "GetDesc1")) return false;
            if (!(info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(make_device(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())))) break;
        }
        if (!device.p) {
            ComPtr<IDXGIAdapter> warp;
            if (!Check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter") ||
                !Check(make_device(warp.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "CreateDevice")) return false;
        }
        Module module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        if (!module.p) return false;
        address = GetProcAddress(module.p, "D3DCompile");
        CompileFn compile = nullptr;
        static_assert(sizeof(compile) == sizeof(address), "unexpected function pointer size");
        std::memcpy(&compile, &address, sizeof(compile));
        if (!compile) return false;
        static const char vs_source[] =
            "cbuffer Camera:register(b0){row_major float4x4 view;row_major float4x4 proj;float2 size;}"
            "cbuffer Instance:register(b2){row_major float4x4 world;}"
            // Wine's DXBC-to-SPIR-V translation requires the varying before SV_Position.
            "struct Output{float4 uv:TEXCOORD0;float4 position:SV_Position;};"
            "Output main(float3 p:POSITION,float2 uv:TEXCOORD0){Output o;"
            "o.position=mul(mul(mul(float4(p,1),world),view),proj);"
            "o.position.x+=o.position.w/size.x;o.position.y-=o.position.w/size.y;o.uv=float4(uv,0,0);return o;}";
        static const char ps_source[] =
            "cbuffer Material:register(b1){float4 color;}float4 main():SV_Target{return color;}";
        static const char texture_ps_source[] =
            "Texture2D image:register(t0);SamplerState image_sampler:register(s0);"
            "float4 main(float4 uv:TEXCOORD0):SV_Target{return image.Sample(image_sampler,uv.xy);}";
        ComPtr<ID3DBlob> vs, ps, texture_ps, lit_vs, lit_ps, lit_texture_ps, lit_fixture_ps, tone_vs, tone_ps;
        if (!Compile(compile, vs_source, "vs_5_0", vs) || !Compile(compile, ps_source, "ps_5_0", ps)) return false;
        if (materials && (!ValidMaterials(*materials) || !Compile(compile,texture_ps_source,"ps_5_0",texture_ps))) return false;
        const std::string untextured=std::string("#define TEXTURED 0\n#define TEST_NORMAL_MAP 0\n")+shaders::pixel;
        const std::string textured=std::string("#define TEXTURED 1\n#define TEST_NORMAL_MAP 0\n")+shaders::pixel;
        const std::string fixture=std::string("#define TEXTURED 1\n#define TEST_NORMAL_MAP 1\n")+shaders::pixel;
        if (!Compile(compile,shaders::vertex,"vs_5_0",lit_vs) || !Compile(compile,untextured.c_str(),"ps_5_0",lit_ps) ||
            !Compile(compile,shaders::tone_vertex,"vs_5_0",tone_vs) || !Compile(compile,shaders::tone_pixel,"ps_5_0",tone_ps) ||
            (materials && (!Compile(compile,textured.c_str(),"ps_5_0",lit_texture_ps) ||
                           !Compile(compile,fixture.c_str(),"ps_5_0",lit_fixture_ps)))) return false;

        D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateQueue") ||
            !Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateAllocator") ||
            !Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence")) return false;
        event.p = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event.p) return false;
        D3D12_DESCRIPTOR_RANGE albedo_range{};albedo_range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        albedo_range.NumDescriptors=4;albedo_range.BaseShaderRegister=0;
        albedo_range.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE hdr_range{};hdr_range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        hdr_range.NumDescriptors=1;hdr_range.BaseShaderRegister=4;
        hdr_range.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[6]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 34;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1; params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister = 2; params[2].Constants.Num32BitValues = 16;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges=1;params[3].DescriptorTable.pDescriptorRanges=&albedo_range;
        params[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor.ShaderRegister=3;
        params[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges=1;params[5].DescriptorTable.pDescriptorRanges=&hdr_range;
        params[5].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        sampler.ShaderRegister=0;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 6; desc.pParameters = params;
        desc.NumStaticSamplers=1;desc.pStaticSamplers=&sampler;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, errors;
        if (!Check(serialize_root(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()), "SerializeRoot") ||
            !Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root.put())), "CreateRoot")) return false;
        D3D12_INPUT_ELEMENT_DESC input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.p;
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = {input, 2}; pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.SampleMask = UINT_MAX; pso.NumRenderTargets = 1; pso.RTVFormats[0] = kColor; pso.DSVFormat = kDepth;
        pso.SampleDesc.Count = 1;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        auto& blend = pso.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable = TRUE; pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = 0xff;
        pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
        if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(pipeline.put())), "CreatePipeline")) return false;
        if (materials) {
            pso.PS={texture_ps->GetBufferPointer(),texture_ps->GetBufferSize()};
            if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(texture_pipeline.put())),"CreateTexturePipeline")) return false;
        }
        pso.RTVFormats[0]=kHdr;pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(hdr_flat_pipeline.put())),"CreateHdrFlatPipeline")) return false;
        if (materials) {
            pso.PS={texture_ps->GetBufferPointer(),texture_ps->GetBufferSize()};
            if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(hdr_texture_pipeline.put())),"CreateHdrTexturePipeline")) return false;
        }
        D3D12_INPUT_ELEMENT_DESC lit_input[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,20,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        pso.InputLayout={lit_input,3};pso.VS={lit_vs->GetBufferPointer(),lit_vs->GetBufferSize()};
        pso.PS={lit_ps->GetBufferPointer(),lit_ps->GetBufferSize()};
        if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(pbr_pipeline.put())),"CreatePbrPipeline")) return false;
        if (materials) {
            pso.PS={lit_texture_ps->GetBufferPointer(),lit_texture_ps->GetBufferSize()};
            if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(pbr_texture_pipeline.put())),"CreatePbrTexturePipeline")) return false;
            pso.PS={lit_fixture_ps->GetBufferPointer(),lit_fixture_ps->GetBufferSize()};
            if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(pbr_fixture_pipeline.put())),"CreatePbrFixturePipeline")) return false;
        }
        pso.InputLayout={nullptr,0};pso.VS={tone_vs->GetBufferPointer(),tone_vs->GetBufferSize()};
        pso.PS={tone_ps->GetBufferPointer(),tone_ps->GetBufferSize()};pso.RTVFormats[0]=kColor;
        pso.DepthStencilState.DepthEnable=FALSE;pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DSVFormat=DXGI_FORMAT_UNKNOWN;
        if (!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(tone_pipeline.put())),"CreateTonePipeline")) return false;
        if (!Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pipeline.p, IID_PPV_ARGS(list.put())), "CreateList") ||
            !Check(list->Close(), "CloseInitialList")) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 2;
        if (!Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_heap.put())), "CreateRtvHeap")) return false;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;heap.NumDescriptors=1;
        if (!Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(dsv_heap.put())), "CreateDsvHeap")) return false;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=11;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(albedo_heap.put())),"CreateMaterialHeap")) return false;
        albedo_descriptor_size=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!albedo_descriptor_size) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};null_srv.Format=kColor;
        null_srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        null_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;null_srv.Texture2D.MipLevels=1;
        auto null_descriptor=albedo_heap->GetCPUDescriptorHandleForHeapStart();
        for (unsigned i=0;i<11;++i) {
            device->CreateShaderResourceView(nullptr,&null_srv,null_descriptor);null_descriptor.ptr+=albedo_descriptor_size;
        }
        const auto upload_heap=Heap(D3D12_HEAP_TYPE_UPLOAD);
        const auto lighting_desc=Buffer((sizeof(LightingConstants)+255u)&~std::size_t(255u));
        if (!Check(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&lighting_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(lighting_buffer.put())),"CreateLightingBuffer")) return false;
        return !materials || InitializeMaterials(false);
    }

    bool InitializeMaterials(bool pbr_channels) {
        if (!Check(allocator->Reset(),"ResetMaterialAllocator") ||
            !Check(list->Reset(allocator.p,pipeline.p),"ResetMaterialList")) return false;
        for (unsigned i=0;i<2;++i) {
            const unsigned first=pbr_channels ? 1 : 0, end=pbr_channels ? 4 : 1;
            for (unsigned channel=first;channel<end;++channel) {
                const auto& texture=materials->materials[i].channels[channel];
                const auto format=channel==0 ? DXGI_FORMAT_R8G8B8A8_TYPELESS : kColor;
                const auto desc=Image(texture.mips[0].width,texture.mips[0].height,
                                      static_cast<UINT16>(texture.mips.size()),format,D3D12_RESOURCE_FLAG_NONE);
                const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);
                if (!allocation.SizeInBytes || allocation.SizeInBytes==UINT64_MAX ||
                    allocation.SizeInBytes>kMaxMaterialGpuBytes-material_bytes) return false;
                material_bytes+=static_cast<std::size_t>(allocation.SizeInBytes);
                auto& gpu=material_textures[i][channel];
                if (!UploadTexture(device.p,list.p,texture,gpu,format)) return false;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=channel==0 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : kColor;
                srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels=static_cast<UINT>(texture.mips.size());
                auto descriptor=albedo_heap->GetCPUDescriptorHandleForHeapStart();
                descriptor.ptr+=std::size_t(i*5+1+channel)*albedo_descriptor_size;
                device->CreateShaderResourceView(gpu.texture.p,&srv,descriptor);
                if (!channel) {
                    descriptor.ptr-=albedo_descriptor_size;srv.Format=kColor;
                    device->CreateShaderResourceView(gpu.texture.p,&srv,descriptor);
                }
            }
        }
        if (!Check(list->Close(),"CloseMaterialList") || fence_value==UINT64_MAX) return false;
        ID3D12CommandList* lists[]{list.p};pending=true;queue->ExecuteCommandLists(1,lists);
        if (!Check(queue->Signal(fence.p,++fence_value),"SignalMaterialUpload") ||
            fence->GetCompletedValue()==UINT64_MAX ||
            (fence->GetCompletedValue()<fence_value &&
             (!Check(fence->SetEventOnCompletion(fence_value,event.p),"SetMaterialFence") ||
              WaitForSingleObject(event.p,kFenceTimeoutMs)!=WAIT_OBJECT_0)) ||
            fence->GetCompletedValue()==UINT64_MAX || fence->GetCompletedValue()<fence_value) return false;
        pending=false;
        for (auto& material:material_textures) for (auto& item:material) Release(item.upload);
        if (pbr_channels) pbr_materials_ready=true;
        return Check(device->GetDeviceRemovedReason(),"MaterialDeviceStatus");
    }

    bool Resize(UINT w, UINT h) {
        if (w == width && h == height && target.p && depth.p) return true;
        Release(target); Release(hdr); Release(depth); Release(readback); Release(backbuffer); readback_bytes = 0;
        width = height = 0;
        if (window) {
            if (!swapchain.p) {
                ComPtr<IDXGISwapChain1> created;
                DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = w; sd.Height = h; sd.Format = kColor;
                sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                if (!Check(factory->CreateSwapChainForHwnd(queue.p, window, &sd, nullptr, nullptr, created.put()), "CreateSwapChain") ||
                    !Check(created->QueryInterface(IID_PPV_ARGS(swapchain.put())), "QuerySwapChain3")) return false;
                (void)factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            } else if (!Check(swapchain->ResizeBuffers(2, w, h, kColor, 0), "ResizeSwapChain")) return false;
        }
        const auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT);
        const auto color_desc = Image(w, h, 1, kColor, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto depth_desc = Image(w, h, 1, kDepth, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE color_clear{}; color_clear.Format = kColor;
        color_clear.Color[0] = 22.0f/255; color_clear.Color[1] = 24.0f/255;
        color_clear.Color[2] = 28.0f/255; color_clear.Color[3] = 1;
        D3D12_CLEAR_VALUE depth_clear{}; depth_clear.Format = kDepth; depth_clear.DepthStencil.Depth = 1;
        if (!Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                &color_clear, IID_PPV_ARGS(target.put())), "CreateTarget") ||
            !Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &depth_clear, IID_PPV_ARGS(depth.put())), "CreateDepth")) return false;
        auto rtv=rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.p,nullptr,rtv);
        device->CreateDepthStencilView(depth.p, nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
        width = w; height = h; return true;
    }

    bool EnsureHdr() {
        if (hdr.p) return true;
        if (!width || !height) return false;
        const auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc=Image(width,height,1,kHdr,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clear{};clear.Format=kHdr;clear.Color[3]=1;
        if (!Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,
                   &clear,IID_PPV_ARGS(hdr.put())),"CreateHdrTarget")) return false;
        auto rtv=rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(hdr.p,nullptr,rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC hdr_srv{};hdr_srv.Format=kHdr;hdr_srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        hdr_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;hdr_srv.Texture2D.MipLevels=1;
        auto descriptor=albedo_heap->GetCPUDescriptorHandleForHeapStart();descriptor.ptr+=10u*albedo_descriptor_size;
        device->CreateShaderResourceView(hdr.p,&hdr_srv,descriptor);
        return true;
    }

    bool EnsureVertices(std::size_t bytes) {
        if (!bytes || bytes <= vertex_capacity) return true;
        Release(vertices); vertex_capacity = 0;
        const auto heap = Heap(D3D12_HEAP_TYPE_UPLOAD); const auto desc = Buffer(bytes);
        if (!Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(vertices.put())), "CreateVertices")) return false;
        vertex_capacity = bytes; return true;
    }
    bool EnsureReadback() {
        if (readback.p) return true;
        const auto desc = target->GetDesc(); UINT rows = 0; UINT64 row_size = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &readback_bytes);
        if (rows != height || row_size != UINT64(width)*4 || footprint.Footprint.RowPitch < row_size ||
            !readback_bytes || readback_bytes > SIZE_MAX || footprint.Offset > readback_bytes ||
            row_size > readback_bytes-footprint.Offset ||
            UINT64(height-1) > (readback_bytes-footprint.Offset-row_size)/footprint.Footprint.RowPitch) return false;
        const auto heap = Heap(D3D12_HEAP_TYPE_READBACK); const auto buffer = Buffer(readback_bytes);
        return Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())), "CreateReadback");
    }

    bool BeginCommands(const Frame& f,bool lit=false) {
        if (!Check(allocator->Reset(), "ResetAllocator") || !Check(list->Reset(allocator.p, pipeline.p), "ResetList")) return false;
        list->SetGraphicsRootSignature(root.p);
        list->SetGraphicsRoot32BitConstants(0, 16, f.view, 0);
        list->SetGraphicsRoot32BitConstants(0, 16, f.projection, 16);
        const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        list->SetGraphicsRoot32BitConstants(2, 16, identity.data(), 0);
        const float size[]{static_cast<float>(width), static_cast<float>(height)};
        list->SetGraphicsRoot32BitConstants(0, 2, size, 32);
        const D3D12_VIEWPORT viewport{0,0,size[0],size[1],f.min_depth,f.max_depth};
        const D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
        auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        if (lit) rtv.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clear[]{lit ? 0.0f : 22.0f/255,lit ? 0.0f : 24.0f/255,lit ? 0.0f : 28.0f/255,1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        lit_commands=lit;return true;
    }
    bool EndCommands(bool wants_readback) {
        if (lit_commands) {
            Transition(list.p,hdr.p,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const auto rtv=rtv_heap->GetCPUDescriptorHandleForHeapStart();list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
            ID3D12DescriptorHeap* heaps[]{albedo_heap.p};list->SetDescriptorHeaps(1,heaps);
            auto handle=albedo_heap->GetGPUDescriptorHandleForHeapStart();handle.ptr+=std::uint64_t(10)*albedo_descriptor_size;
            list->SetGraphicsRootDescriptorTable(5,handle);list->SetPipelineState(tone_pipeline.p);
            list->DrawInstanced(3,1,0,0);
            Transition(list.p,hdr.p,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (swapchain.p || wants_readback) Transition(list.p, target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (swapchain.p) {
            if (!Check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(backbuffer.put())), "GetBackBuffer")) return false;
            Transition(list.p, backbuffer.p, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(backbuffer.p, target.p);
            Transition(list.p, backbuffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
        if (wants_readback) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = target.p; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        if (swapchain.p || wants_readback) Transition(list.p, target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!Check(list->Close(), "CloseList") || fence_value == UINT64_MAX) return false;
        prepared = true; prepared_readback = wants_readback; return true;
    }
    bool Prepare(const Frame& f, bool wants_readback) {
        if (failed || pending || prepared || !Valid(f) || !Resize(f.width, f.height)) return false;
        const auto bytes = f.vertices.size()*sizeof(Vertex);
        if (!EnsureVertices(bytes) || (wants_readback && !EnsureReadback())) return false;
        if (bytes) {
            void* mapped = nullptr; const D3D12_RANGE no_read{0,0};
            if (!Check(vertices->Map(0, &no_read, &mapped), "MapVertices") || !mapped) return false;
            std::memcpy(mapped, f.vertices.data(), bytes);
            const D3D12_RANGE written{0, bytes}; vertices->Unmap(0, &written);
        }
        if (!BeginCommands(f)) return false;
        if (bytes) {
            const D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(),static_cast<UINT>(bytes),sizeof(Vertex)};
            list->IASetVertexBuffers(0, 1, &vb);
            for (const auto& draw : f.draws) {
                list->SetGraphicsRoot32BitConstants(1, 4, draw.ambient_color, 0);
                list->DrawInstanced(draw.vertex_count, 1, draw.first_vertex, 0);
            }
        }
        return EndCommands(wants_readback);
    }
    bool UploadBuffer(const void* data, std::size_t bytes, ComPtr<ID3D12Resource>& output) {
        if (!data || !bytes || bytes>UINT_MAX) return false;
        const auto heap=Heap(D3D12_HEAP_TYPE_UPLOAD);const auto desc=Buffer(bytes);
        if (!Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,IID_PPV_ARGS(output.put())),"CreateResidentBuffer")) return false;
        const D3D12_RANGE no_read{0,0};void* mapped=nullptr;
        if (!Check(output->Map(0,&no_read,&mapped),"MapResidentBuffer") || !mapped) return false;
        std::memcpy(mapped,data,bytes);const D3D12_RANGE written{0,bytes};output->Unmap(0,&written);return true;
    }
    bool EnsureCached(const std::shared_ptr<const Resource>& resource,
                      const std::unordered_set<std::string>& required) {
        if (!resource || resource->id.empty()) return false;
        const auto found=cache.find(resource->id);
        if (found!=cache.end()) {
            if (found->second->source!=resource) return false;
            found->second->last_use=cache_serial;return true;
        }
        const auto& geometry=resource->geometry;
        if (geometry.positions.empty() || geometry.positions.size()>1000000 || geometry.indices.empty() ||
            geometry.indices.size()>3000000 || geometry.positions.size()>UINT_MAX/sizeof(PackedVertex) ||
            (!geometry.uv0.empty() && geometry.uv0.size()!=geometry.positions.size()) ||
            (!geometry.normals.empty() && geometry.normals.size()!=geometry.positions.size()) ||
            geometry.indices.size()>UINT_MAX/sizeof(std::uint32_t)) return false;
        const auto vertex_bytes=geometry.positions.size()*sizeof(PackedVertex);
        const auto index_bytes=geometry.indices.size()*sizeof(std::uint32_t);
        const auto vertex_desc=Buffer(vertex_bytes), index_desc=Buffer(index_bytes);
        const auto vertex_allocation=device->GetResourceAllocationInfo(0,1,&vertex_desc);
        const auto index_allocation=device->GetResourceAllocationInfo(0,1,&index_desc);
        if (!vertex_allocation.SizeInBytes || !index_allocation.SizeInBytes ||
            vertex_allocation.SizeInBytes==UINT64_MAX || index_allocation.SizeInBytes==UINT64_MAX ||
            vertex_allocation.SizeInBytes>SIZE_MAX || index_allocation.SizeInBytes>SIZE_MAX-vertex_allocation.SizeInBytes) return false;
        const auto bytes=static_cast<std::size_t>(vertex_allocation.SizeInBytes+index_allocation.SizeInBytes);
        if (bytes>cache_budget) return false;
        while (bytes>cache_budget-cache_bytes || cache.size()>=4096) {
            auto victim=cache.end();
            for (auto it=cache.begin();it!=cache.end();++it)
                if (!required.count(it->first) && (victim==cache.end() || it->second->last_use<victim->second->last_use)) victim=it;
            if (victim==cache.end()) return false;
            cache_bytes-=victim->second->bytes;cache.erase(victim);++evictions;
        }
        auto gpu=std::make_unique<CachedGeometry>();gpu->source=resource;gpu->bytes=bytes;gpu->last_use=cache_serial;
        gpu->has_normals=geometry.normals.size()==geometry.positions.size();
        gpu->low=gpu->high=geometry.positions[0];
        std::vector<PackedVertex> vertices;vertices.reserve(geometry.positions.size());
        for (std::size_t n=0;n<geometry.positions.size();++n) {
            const auto& p=geometry.positions[n];
            const Vec2 uv=geometry.uv0.empty() ? Vec2{} : geometry.uv0[n];
            const Vec3 normal=geometry.normals.empty() ? Vec3{} : geometry.normals[n];
            if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return false;
            float normal_length=0;
            for (auto v:normal) {if (!std::isfinite(v)) return false;normal_length+=v*v;}
            if (!std::isfinite(normal_length) || normal_length<1e-12f) gpu->has_normals=false;
            vertices.push_back({p,uv,normal});
            for (unsigned i=0;i<3;++i) {
                if (!std::isfinite(p[i])) return false;
                gpu->low[i]=std::min(gpu->low[i],p[i]);gpu->high[i]=std::max(gpu->high[i],p[i]);
            }
        }
        for (auto index:geometry.indices) if (index>=geometry.positions.size()) return false;
        if (!UploadBuffer(vertices.data(),vertex_bytes,gpu->vertices) ||
            !UploadBuffer(geometry.indices.data(),index_bytes,gpu->indices)) return false;
        cache_bytes+=bytes;cache.emplace(resource->id,std::move(gpu));++uploads;return true;
    }
    bool ValidWorldBounds(const CachedGeometry& geometry,const Matrix& world) const {
        for (unsigned corner=0;corner<8;++corner) {
            double input[3]{};
            for (unsigned i=0;i<3;++i) input[i]=(corner&(1u<<i)) ? geometry.high[i] : geometry.low[i];
            for (unsigned axis=0;axis<3;++axis) {
                const double value=input[0]*world[axis]+input[1]*world[4+axis]+input[2]*world[8+axis]+world[12+axis];
                if (!std::isfinite(value) || std::fabs(value)>1000000) return false;
            }
        }
        return true;
    }
    bool PrepareResident(const ResidentFlatFrame& f,bool wants_readback,const LightingConstants* lighting=nullptr,
                         bool synthetic_normal_map=false) {
        if (failed || pending || prepared || !f.map_epoch || !f.device_epoch || !f.continuity || !Valid(f.camera) ||
            !f.camera.vertices.empty() || !f.camera.draws.empty() || f.draws.size()>scene::kMaxDraws ||
            !Resize(f.camera.width,f.camera.height) || (lighting && !EnsureHdr()) ||
            (wants_readback && !EnsureReadback())) return false;
        if (cache_map!=f.map_epoch || cache_device!=f.device_epoch || cache_continuity!=f.continuity) {
            cache.clear();cache_bytes=0;cache_map=f.map_epoch;cache_device=f.device_epoch;cache_continuity=f.continuity;
        }
        if (cache_serial==UINT64_MAX) return false;
        ++cache_serial;
        std::unordered_set<std::string> required;
        std::size_t total_indices=0;
        for (const auto& draw:f.draws) {
            if (!draw.resource || draw.resource->id.empty() || draw.section>=draw.resource->geometry.sections.size()) return false;
            const auto& section=draw.resource->geometry.sections[draw.section];
            if (!section.index_count || section.index_count%3 || !draw.index_count || draw.index_count%3 ||
                draw.first_index<section.first_index || (draw.first_index-section.first_index)%3 ||
                std::uint64_t{draw.first_index}+draw.index_count>std::uint64_t{section.first_index}+section.index_count ||
                std::uint64_t{section.first_index}+section.index_count>draw.resource->geometry.indices.size() ||
                draw.index_count>scene::kMaxTriangles*3-total_indices ||
                !std::all_of(draw.transform.begin(),draw.transform.end(),[](float v){return std::isfinite(v);}) ||
                std::fabs(draw.transform[3])>1e-6f || std::fabs(draw.transform[7])>1e-6f ||
                std::fabs(draw.transform[11])>1e-6f || std::fabs(draw.transform[15]-1)>1e-6f ||
                !std::all_of(draw.color.begin(),draw.color.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;})) return false;
            required.insert(draw.resource->id);
            total_indices+=draw.index_count;
        }
        for (const auto& draw:f.draws) if (!EnsureCached(draw.resource,required)) return false;
        if (lighting) {
            void* mapped=nullptr;const D3D12_RANGE no_read{0,0};
            if (!Check(lighting_buffer->Map(0,&no_read,&mapped),"MapLighting") || !mapped) return false;
            std::memcpy(mapped,lighting,sizeof(*lighting));const D3D12_RANGE written{0,sizeof(*lighting)};
            lighting_buffer->Unmap(0,&written);
        }
        if (!BeginCommands(f.camera,lighting!=nullptr)) return false;
        if (lighting) list->SetGraphicsRootConstantBufferView(4,lighting_buffer->GetGPUVirtualAddress());
        if (albedo_heap.p) {ID3D12DescriptorHeap* heaps[]{albedo_heap.p};list->SetDescriptorHeaps(1,heaps);}
        textured_draws=0;pbr_draws=0;
        for (const auto& draw:f.draws) {
            const auto& gpu=*cache.at(draw.resource->id);
            if (!ValidWorldBounds(gpu,draw.transform)) return false;
            const auto& geometry=gpu.source->geometry;
            const D3D12_VERTEX_BUFFER_VIEW vb{gpu.vertices->GetGPUVirtualAddress(),
                static_cast<UINT>(geometry.positions.size()*sizeof(PackedVertex)),sizeof(PackedVertex)};
            const D3D12_INDEX_BUFFER_VIEW ib{gpu.indices->GetGPUVirtualAddress(),
                static_cast<UINT>(geometry.indices.size()*sizeof(std::uint32_t)),DXGI_FORMAT_R32_UINT};
            list->IASetVertexBuffers(0,1,&vb);list->IASetIndexBuffer(&ib);
            list->SetGraphicsRoot32BitConstants(2,16,draw.transform.data(),0);
            list->SetGraphicsRoot32BitConstants(1,4,draw.color.data(),0);
            unsigned texture_index=2;
            if (materials && draw.material_id && geometry.uv0.size()==geometry.positions.size())
                for (unsigned i=0;i<2;++i) if (*draw.material_id==materials->materials[i].id) texture_index=i;
            const auto& w=draw.transform;
            const double determinant=double(w[0])*(double(w[5])*w[10]-double(w[6])*w[9])-
                double(w[1])*(double(w[4])*w[10]-double(w[6])*w[8])+
                double(w[2])*(double(w[4])*w[9]-double(w[5])*w[8]);
            const bool can_light=lighting && gpu.has_normals && std::isfinite(determinant) && std::fabs(determinant)>1e-10;
            if (can_light) ++pbr_draws;
            if (texture_index<2) {
                auto handle=albedo_heap->GetGPUDescriptorHandleForHeapStart();
                handle.ptr+=std::uint64_t(texture_index*5+(can_light ? 1 : 0))*albedo_descriptor_size;
                list->SetPipelineState(can_light ? (synthetic_normal_map ? pbr_fixture_pipeline.p : pbr_texture_pipeline.p) :
                                       lighting ? hdr_texture_pipeline.p : texture_pipeline.p);
                list->SetGraphicsRootDescriptorTable(3,handle);++textured_draws;
            } else list->SetPipelineState(can_light ? pbr_pipeline.p : lighting ? hdr_flat_pipeline.p : pipeline.p);
            list->DrawIndexedInstanced(draw.index_count,1,draw.first_index,0,0);
        }
        return EndCommands(wants_readback);
    }
    bool Submit() {
        if (failed || pending || !prepared) return false;
        prepared = false;
        ID3D12CommandList* lists[]{list.p}; pending = true; queue->ExecuteCommandLists(1, lists);
        const auto next = ++fence_value;
        return Check(queue->Signal(fence.p, next), "Signal");
    }
    bool Finish(std::vector<std::uint8_t>* pixels) {
        if (failed || !pending || (pixels && !prepared_readback)) return false;
        if (fence->GetCompletedValue() == UINT64_MAX ||
            (fence->GetCompletedValue() < fence_value &&
             (!Check(fence->SetEventOnCompletion(fence_value, event.p), "SetFenceEvent") ||
              WaitForSingleObject(event.p, kFenceTimeoutMs) != WAIT_OBJECT_0)) ||
            fence->GetCompletedValue() == UINT64_MAX || fence->GetCompletedValue() < fence_value) return false;
        pending = false;
        Release(backbuffer);
        if (!Check(device->GetDeviceRemovedReason(), "DeviceStatus")) return false;
        if (pixels) {
            std::vector<std::uint8_t> result(std::size_t(width)*height*4);
            const D3D12_RANGE read{0,static_cast<SIZE_T>(readback_bytes)}; void* mapped = nullptr;
            if (!Check(readback->Map(0, &read, &mapped), "MapReadback") || !mapped) return false;
            for (UINT row=0; row<height; ++row)
                std::memcpy(result.data()+std::size_t(row)*width*4,
                    static_cast<const std::uint8_t*>(mapped)+footprint.Offset+std::size_t(row)*footprint.Footprint.RowPitch,
                    std::size_t(width)*4);
            const D3D12_RANGE no_write{0,0}; readback->Unmap(0, &no_write); pixels->swap(result);
        }
        prepared_readback = false;
        return true;
    }
    void Cancel() noexcept { if (prepared && !pending) { prepared = false; prepared_readback = false; Release(backbuffer); } }
    bool RetireResidents() noexcept {
        if (pending || prepared) return false;
        cache.clear();cache_bytes=0;cache_map=cache_device=cache_continuity=0;return true;
    }
    FlatPresent Present() {
        if (failed || pending || prepared) return FlatPresent::Failed;
        if (!swapchain.p) return FlatPresent::Presented;
        const HRESULT hr=swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
        return SUCCEEDED(hr) ? FlatPresent::Presented : hr==DXGI_ERROR_WAS_STILL_DRAWING ? FlatPresent::Busy : FlatPresent::Failed;
    }
};

FlatD3D12::FlatD3D12(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FlatD3D12::~FlatD3D12() {
    // Never terminate the host process or release resources still in flight.
    if (impl_ && impl_->pending) (void)impl_.release();
}
std::unique_ptr<FlatD3D12> FlatD3D12::Create(HWND window, std::size_t cache_budget,
                                             std::shared_ptr<const BedroomMaterialLibrary> materials) noexcept {
    std::unique_ptr<Impl> impl;
    try {
        if (!cache_budget || cache_budget>512u*1024u*1024u) return {};
        impl=std::make_unique<Impl>();impl->window=window;impl->cache_budget=cache_budget;impl->materials=std::move(materials);
        if (!impl->Initialize()) {if (impl->pending) (void)impl.release();return {};}
        return std::unique_ptr<FlatD3D12>(new FlatD3D12(std::move(impl)));
    } catch (...) {if (impl && impl->pending) (void)impl.release();return {};}
}
bool FlatD3D12::PrepareResident(const ResidentFlatFrame& frame, bool readback) noexcept {
    try {
        if (!impl_ || impl_->failed) return false;
        if (impl_->PrepareResident(frame,readback)) return true;
        impl_->failed=true;return false;
    } catch (...) { if (impl_) impl_->failed=true;return false; }
}
bool FlatD3D12::PrepareLit(const Snapshot& snapshot,const ResidentFlatFrame& frame,bool readback,
                           bool synthetic_normal_map) noexcept {
    try {
        if (!impl_ || impl_->failed || (synthetic_normal_map && snapshot.provenance.origin!=FrameOrigin::Synthetic) ||
            frame.map_epoch!=snapshot.map_epoch || frame.device_epoch!=snapshot.device_epoch ||
            frame.continuity!=snapshot.continuity || frame.frame_id!=snapshot.frame_id ||
            frame.render_pass!=snapshot.render_pass || frame.stage!=snapshot.stage ||
            frame.map!=snapshot.map || frame.map_sha256!=snapshot.map_sha256 ||
            frame.provenance.origin!=snapshot.provenance.origin || frame.provenance.session_id!=snapshot.provenance.session_id ||
            frame.provenance.capture_sha256!=snapshot.provenance.capture_sha256 ||
            frame.viewport_x!=snapshot.camera.x || frame.viewport_y!=snapshot.camera.y ||
            frame.camera.width!=snapshot.camera.width || frame.camera.height!=snapshot.camera.height ||
            frame.camera.min_depth!=snapshot.camera.min_depth || frame.camera.max_depth!=snapshot.camera.max_depth ||
            std::memcmp(frame.camera.view,snapshot.camera.view.data(),sizeof(frame.camera.view)) ||
            std::memcmp(frame.camera.projection,snapshot.camera.projection.data(),sizeof(frame.camera.projection))) return false;
        VerifiedLighting verified;
        if (BuildVerifiedLighting(snapshot,verified)!=LightingResult::Ready || !LightingMatchesSnapshot(snapshot,verified)) return false;
        if (impl_->pending || impl_->prepared) return false;
        // Flat-only sessions retain the old two-albedo upload footprint. The
        // remaining channels are uploaded once, outside the submission lock.
        if (impl_->materials && !impl_->pbr_materials_ready && !impl_->InitializeMaterials(true)) {
            impl_->failed=true;return false;
        }
        LightingConstants constants;
        constants.camera_scale={verified.camera_position[0],verified.camera_position[1],verified.camera_position[2],verified.meters_per_unit};
        constants.options={static_cast<float>(verified.lights.size()),synthetic_normal_map ? 1.0f : 0.0f,0,0};
        for (std::size_t i=0;i<verified.lights.size();++i) {
            const auto& light=verified.lights[i];
            constants.position_range[i]={light.position[0],light.position[1],light.position[2],light.influence_radius_m};
            constants.color_intensity[i]={light.color_linear[0],light.color_linear[1],light.color_linear[2],light.intensity};
            constants.direction_outer[i]={light.direction[0],light.direction[1],light.direction[2],light.cos_outer};
            constants.params[i]={light.source_radius_m,light.cos_inner,light.kind==LightKind::Spot ? 1.0f : 0.0f,0};
        }
        if (impl_->PrepareResident(frame,readback,&constants,synthetic_normal_map)) return true;
        impl_->failed=true;return false;
    } catch (...) { if (impl_) impl_->failed=true;return false; }
}
FlatCacheStats FlatD3D12::cache_stats() const noexcept {
    if (!impl_) return {};
    return {impl_->uploads,impl_->evictions,impl_->cache.size(),impl_->cache_bytes,impl_->textured_draws,impl_->pbr_draws};
}
bool FlatD3D12::Prepare(const scene::Frame& frame, bool readback) noexcept {
    try {
        if (!impl_ || impl_->failed) return false;
        if (impl_->Prepare(frame, readback)) return true;
        impl_->failed = true; return false;
    } catch (...) { if (impl_) impl_->failed = true; return false; }
}
bool FlatD3D12::Submit() noexcept {
    try {
        if (!impl_ || impl_->failed) return false;
        if (impl_->Submit()) return true;
        impl_->failed = true; return false;
    } catch (...) { if (impl_) impl_->failed = true; return false; }
}
bool FlatD3D12::Finish(std::vector<std::uint8_t>* pixels) noexcept {
    try {
        if (!impl_ || impl_->failed) return false;
        if (impl_->Finish(pixels)) return true;
        impl_->failed = true; return false;
    } catch (...) { if (impl_) impl_->failed = true; return false; }
}
void FlatD3D12::Cancel() noexcept { if (impl_) impl_->Cancel(); }
bool FlatD3D12::RetireResidents() noexcept { return impl_ && !impl_->failed && impl_->RetireResidents(); }
FlatPresent FlatD3D12::Present() noexcept {
    try {
        if (!impl_ || impl_->failed) return FlatPresent::Failed;
        const auto result=impl_->Present();
        if (result==FlatPresent::Failed) impl_->failed=true;
        return result;
    } catch (...) { if (impl_) impl_->failed=true;return FlatPresent::Failed; }
}
bool FlatD3D12::Render(const scene::Frame& frame, std::vector<std::uint8_t>* pixels) noexcept {
    return Prepare(frame, pixels != nullptr) && Submit() && Finish(pixels) && Present()!=FlatPresent::Failed;
}
} // namespace t3render::live
