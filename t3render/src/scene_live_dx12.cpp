#include "scene_live_dx12.hpp"
#include "scene_d3d12_common.hpp"

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
constexpr DWORD kFenceTimeoutMs = 10000;

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
} // namespace

struct FlatD3D12::Impl {
    struct CachedGeometry {
        std::shared_ptr<const Resource> source;
        ComPtr<ID3D12Resource> vertices, indices;
        std::size_t bytes=0;
        std::uint64_t last_use=0;
        Vec3 low{}, high{};
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
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    ComPtr<ID3D12Resource> target, depth, vertices, readback, backbuffer;
    ComPtr<IDXGISwapChain3> swapchain;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readback_bytes = 0, fence_value = 0;
    std::size_t vertex_capacity = 0;
    std::size_t cache_budget=0, cache_bytes=0;
    std::uint64_t cache_map=0, cache_device=0, cache_continuity=0, cache_serial=0, uploads=0, evictions=0;
    std::unordered_map<std::string,std::unique_ptr<CachedGeometry>> cache;
    UINT width = 0, height = 0;
    bool failed = false, pending = false, prepared = false, prepared_readback = false;

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
            "float4 main(float3 p:POSITION):SV_Position{float4 v=mul(mul(mul(float4(p,1),world),view),proj);"
            "v.x+=v.w/size.x;v.y-=v.w/size.y;return v;}";
        static const char ps_source[] =
            "cbuffer Material:register(b1){float4 color;}float4 main():SV_Target{return color;}";
        ComPtr<ID3DBlob> vs, ps;
        if (!Compile(compile, vs_source, "vs_5_0", vs) || !Compile(compile, ps_source, "ps_5_0", ps)) return false;

        D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateQueue") ||
            !Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateAllocator") ||
            !Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence")) return false;
        event.p = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event.p) return false;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 34;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1; params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister = 2; params[2].Constants.Num32BitValues = 16;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 3; desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, errors;
        if (!Check(serialize_root(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()), "SerializeRoot") ||
            !Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root.put())), "CreateRoot")) return false;
        D3D12_INPUT_ELEMENT_DESC input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.p;
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = {input, 1}; pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
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
        if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(pipeline.put())), "CreatePipeline") ||
            !Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pipeline.p, IID_PPV_ARGS(list.put())), "CreateList") ||
            !Check(list->Close(), "CloseInitialList")) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 1;
        if (!Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_heap.put())), "CreateRtvHeap")) return false;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        return Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(dsv_heap.put())), "CreateDsvHeap");
    }

    bool Resize(UINT w, UINT h) {
        if (w == width && h == height && target.p && depth.p) return true;
        Release(target); Release(depth); Release(readback); Release(backbuffer); readback_bytes = 0;
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
        device->CreateRenderTargetView(target.p, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateDepthStencilView(depth.p, nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
        width = w; height = h; return true;
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

    bool BeginCommands(const Frame& f) {
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
        const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clear[]{22.0f/255,24.0f/255,28.0f/255,1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        return true;
    }
    bool EndCommands(bool wants_readback) {
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
            geometry.indices.size()>3000000 || geometry.positions.size()>UINT_MAX/sizeof(Vec3) ||
            geometry.indices.size()>UINT_MAX/sizeof(std::uint32_t)) return false;
        const auto vertex_bytes=geometry.positions.size()*sizeof(Vec3);
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
        gpu->low=gpu->high=geometry.positions[0];
        for (const auto& p:geometry.positions) for (unsigned i=0;i<3;++i) {
            if (!std::isfinite(p[i])) return false;
            gpu->low[i]=std::min(gpu->low[i],p[i]);gpu->high[i]=std::max(gpu->high[i],p[i]);
        }
        for (auto index:geometry.indices) if (index>=geometry.positions.size()) return false;
        if (!UploadBuffer(geometry.positions.data(),vertex_bytes,gpu->vertices) ||
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
    bool PrepareResident(const ResidentFlatFrame& f,bool wants_readback) {
        if (failed || pending || prepared || !f.map_epoch || !f.device_epoch || !f.continuity || !Valid(f.camera) ||
            !f.camera.vertices.empty() || !f.camera.draws.empty() || f.draws.size()>scene::kMaxDraws ||
            !Resize(f.camera.width,f.camera.height) || (wants_readback && !EnsureReadback())) return false;
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
            if (!section.index_count || section.index_count%3 ||
                std::uint64_t{section.first_index}+section.index_count>draw.resource->geometry.indices.size() ||
                section.index_count>scene::kMaxTriangles*3-total_indices ||
                !std::all_of(draw.transform.begin(),draw.transform.end(),[](float v){return std::isfinite(v);}) ||
                std::fabs(draw.transform[3])>1e-6f || std::fabs(draw.transform[7])>1e-6f ||
                std::fabs(draw.transform[11])>1e-6f || std::fabs(draw.transform[15]-1)>1e-6f ||
                !std::all_of(draw.color.begin(),draw.color.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;})) return false;
            required.insert(draw.resource->id);
            total_indices+=section.index_count;
        }
        for (const auto& draw:f.draws) if (!EnsureCached(draw.resource,required)) return false;
        if (!BeginCommands(f.camera)) return false;
        for (const auto& draw:f.draws) {
            const auto& gpu=*cache.at(draw.resource->id);
            if (!ValidWorldBounds(gpu,draw.transform)) return false;
            const auto& geometry=gpu.source->geometry;const auto& section=geometry.sections[draw.section];
            const D3D12_VERTEX_BUFFER_VIEW vb{gpu.vertices->GetGPUVirtualAddress(),
                static_cast<UINT>(geometry.positions.size()*sizeof(Vec3)),sizeof(Vec3)};
            const D3D12_INDEX_BUFFER_VIEW ib{gpu.indices->GetGPUVirtualAddress(),
                static_cast<UINT>(geometry.indices.size()*sizeof(std::uint32_t)),DXGI_FORMAT_R32_UINT};
            list->IASetVertexBuffers(0,1,&vb);list->IASetIndexBuffer(&ib);
            list->SetGraphicsRoot32BitConstants(2,16,draw.transform.data(),0);
            list->SetGraphicsRoot32BitConstants(1,4,draw.color.data(),0);
            list->DrawIndexedInstanced(section.index_count,1,section.first_index,0,0);
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
std::unique_ptr<FlatD3D12> FlatD3D12::Create(HWND window, std::size_t cache_budget) noexcept {
    try {
        if (!cache_budget || cache_budget>512u*1024u*1024u) return {};
        auto impl = std::make_unique<Impl>(); impl->window = window;impl->cache_budget=cache_budget;
        if (!impl->Initialize()) return {};
        return std::unique_ptr<FlatD3D12>(new FlatD3D12(std::move(impl)));
    } catch (...) { return {}; }
}
bool FlatD3D12::PrepareResident(const ResidentFlatFrame& frame, bool readback) noexcept {
    try {
        if (!impl_ || impl_->failed) return false;
        if (impl_->PrepareResident(frame,readback)) return true;
        impl_->failed=true;return false;
    } catch (...) { if (impl_) impl_->failed=true;return false; }
}
FlatCacheStats FlatD3D12::cache_stats() const noexcept {
    if (!impl_) return {};
    return {impl_->uploads,impl_->evictions,impl_->cache.size(),impl_->cache_bytes};
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
