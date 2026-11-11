// Bounded one-triangle D3D12 replay of synthetic D3D8 XYZRHW|DIFFUSE or
// XYZ|DIFFUSE with captured WORLD/VIEW/PROJECTION. Flat opaque color only;
// this is not a general frame reader, D3D8 emulator, or T3 replay.
#ifndef T3RENDER_REPLAY_PARSER_TEST
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 256;
constexpr std::size_t kMaxInputBytes = 4096;
#ifndef T3RENDER_REPLAY_PARSER_TEST
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
#endif
struct Vertex {
    float x, y, z, rhw;
    std::uint32_t argb;
};
static_assert(sizeof(Vertex) == 20, "FVF XYZRHW|DIFFUSE must have a 20-byte stride");
struct Frame {
    std::uint32_t clear_argb = 0;
    Vertex vertices[3]{};
    bool camera = false;
    float matrices[3][16]{}; // D3D8 row-major WORLD, VIEW, PROJECTION
};

#ifndef T3RENDER_REPLAY_PARSER_TEST
bool Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s failed HRESULT=0x%08lx\n", operation,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}
#endif

bool DecimalU32(const std::string& text, std::uint32_t& result) {
    if (text.empty()) return false;
    std::uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<std::uint32_t>(c - '0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    result = value;
    return true;
}

bool AtEnd(std::istringstream& stream) {
    stream >> std::ws;
    return stream.eof();
}

std::istringstream Line(const std::string& line) {
    std::istringstream stream(line);
    stream.imbue(std::locale::classic());
    return stream;
}

bool CameraTriangleValid(const Frame& frame) {
    double projected[3][2]{};
    for (unsigned i = 0; i < 3; ++i) {
        const Vertex& v = frame.vertices[i];
        double p[4] = {v.x, v.y, v.z, 1.0};
        for (unsigned m = 0; m < 3; ++m) {
            double next[4]{};
            for (unsigned col = 0; col < 4; ++col)
                for (unsigned row = 0; row < 4; ++row)
                    next[col] += p[row] * frame.matrices[m][row * 4 + col];
            for (unsigned col = 0; col < 4; ++col) p[col] = next[col];
        }
        for (double value : p)
            if (!std::isfinite(value) || std::fabs(value) > 1e12) return false;
        const double w = p[3];
        if (w <= 1e-5 || p[0] < -w || p[0] > w || p[1] < -w || p[1] > w ||
            p[2] < 0 || p[2] > w) return false;
        projected[i][0] = p[0] / w;
        projected[i][1] = p[1] / w;
    }
    const double area = (projected[1][0] - projected[0][0]) * (projected[2][1] - projected[0][1]) -
                        (projected[1][1] - projected[0][1]) * (projected[2][0] - projected[0][0]);
    return std::fabs(area) > 1e-8;
}

bool ParseFrameBytes(const char* bytes, std::size_t size, Frame& frame) {
    if (!bytes || size > kMaxInputBytes || std::memchr(bytes, '\0', size)) {
        std::fprintf(stderr, "input exceeds limit or contains invalid bytes\n"); return false;
    }
    auto stream = Line(std::string(bytes, size));
    std::string lines[9];
    if (!std::getline(stream, lines[0])) { std::fprintf(stderr, "missing header\n"); return false; }
    if (!lines[0].empty() && lines[0].back() == '\r') lines[0].pop_back();
    if (lines[0] != "T3R_UP_V1" && lines[0] != "T3R_XYZ_V1") {
        std::fprintf(stderr, "invalid header\n"); return false;
    }
    frame.camera = lines[0] == "T3R_XYZ_V1";
    const unsigned line_count = frame.camera ? 9 : 6;
    for (unsigned i = 1; i < line_count; ++i) {
        if (!std::getline(stream, lines[i])) { std::fprintf(stderr, "missing input line\n"); return false; }
        if (!lines[i].empty() && lines[i].back() == '\r') lines[i].pop_back();
    }
    if (!AtEnd(stream)) { std::fprintf(stderr, "trailing input\n"); return false; }
    std::string width, height;
    std::uint32_t parsed_width = 0, parsed_height = 0;
    auto viewport = Line(lines[1]);
    if (!(viewport >> width >> height) || !AtEnd(viewport) || !DecimalU32(width, parsed_width) ||
        !DecimalU32(height, parsed_height) || parsed_width != kWidth || parsed_height != kHeight) {
        std::fprintf(stderr, "viewport must be exactly 256 256\n"); return false;
    }
    std::string clear;
    auto clear_line = Line(lines[2]);
    if (!(clear_line >> clear) || !AtEnd(clear_line) || !DecimalU32(clear, frame.clear_argb)) {
        std::fprintf(stderr, "invalid decimal ARGB clear color\n"); return false;
    }
    if (frame.camera) {
        if ((frame.clear_argb >> 24) != 255) { std::fprintf(stderr, "clear color must be opaque\n"); return false; }
        for (unsigned m = 0; m < 3; ++m) {
            auto line = Line(lines[3 + m]);
            for (float& value : frame.matrices[m]) {
                if (!(line >> value) || !std::isfinite(value) || std::fabs(value) > 10000) {
                    std::fprintf(stderr, "invalid matrix %u\n", m); return false;
                }
            }
            if (!AtEnd(line)) { std::fprintf(stderr, "extra matrix values\n"); return false; }
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        Vertex vertex{};
        std::string color;
        auto line = Line(lines[i + (frame.camera ? 6 : 3)]);
        if (!(line >> vertex.x >> vertex.y >> vertex.z) ||
            (!frame.camera && !(line >> vertex.rhw)) || !(line >> color) || !AtEnd(line) ||
            !DecimalU32(color, vertex.argb) || !std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z)) {
            std::fprintf(stderr, "invalid or out-of-range vertex %u\n", i); return false;
        }
        if (frame.camera) {
            vertex.rhw = 1.0f;
            if (std::fabs(vertex.x) > 10000 || std::fabs(vertex.y) > 10000 || std::fabs(vertex.z) > 10000) {
                std::fprintf(stderr, "camera vertex out of range\n"); return false;
            }
        } else if (!std::isfinite(vertex.rhw) || std::fabs(vertex.x) > 1048576.0f ||
                   std::fabs(vertex.y) > 1048576.0f || vertex.z < 0.0f || vertex.z > 1.0f ||
                   vertex.rhw < 0.000001f || vertex.rhw > 1000000.0f) {
            std::fprintf(stderr, "screen vertex out of range\n"); return false;
        }
        frame.vertices[i] = vertex;
    }
    // The fragment stage uses one captured color rather than interpolating a
    // varying. Reject inputs whose D3D8 shading could differ from that model.
    for (const Vertex& vertex : frame.vertices) {
        if ((!frame.camera && (vertex.z != 0.0f || vertex.rhw != 1.0f)) ||
            (vertex.argb >> 24) != 255 || vertex.argb != frame.vertices[0].argb) {
            std::fprintf(stderr, "only flat opaque color and supported screen-space depth are accepted\n"); return false;
        }
    }
    if (frame.camera && (frame.vertices[0].argb == frame.clear_argb || !CameraTriangleValid(frame))) {
        std::fprintf(stderr, "camera triangle is clear-colored, clipped or degenerate\n"); return false;
    }
    return true;
}

#ifndef T3RENDER_REPLAY_PARSER_TEST
bool ReadFrame(const wchar_t* path, Frame& frame) {
    FILE* input = _wfopen(path, L"rb");
    if (!input) { std::fprintf(stderr, "cannot open input file\n"); return false; }
    char bytes[kMaxInputBytes + 1];
    const std::size_t size = std::fread(bytes, 1, sizeof(bytes), input);
    const bool read_error = std::ferror(input) != 0;
    const bool close_error = std::fclose(input) != 0;
    if (read_error || close_error) { std::fprintf(stderr, "failed to read input file\n"); return false; }
    return ParseFrameBytes(bytes, size, frame);
}

std::string Utf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "<unavailable>";
    std::vector<char> buffer(static_cast<std::size_t>(length));
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer.data(), length, nullptr, nullptr)) return "<unavailable>";
    return std::string(buffer.data());
}

bool SelectDevice(IDXGIFactory4* factory, ComPtr<IDXGIAdapter1>& selected, ComPtr<ID3D12Device>& device) {
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, adapter.put());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (!Check(hr, "EnumAdapters1")) return false;
        DXGI_ADAPTER_DESC1 desc{};
        if (!Check(adapter->GetDesc1(&desc), "GetDesc1")) return false;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ID3D12Device* candidate = nullptr;
        const HRESULT created = D3D12CreateDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&candidate));
        if (FAILED(created) || !candidate) { if (candidate) candidate->Release(); continue; }
        device.value = candidate;
        selected.value = adapter.value;
        adapter.value = nullptr;
        return true;
    }
    std::fprintf(stderr, "No hardware D3D12 device; trying WARP\n");
    ComPtr<IDXGIAdapter> warp;
    return Check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter") &&
           Check(warp->QueryInterface(IID_PPV_ARGS(selected.put())), "WARP QueryInterface") &&
           Check(D3D12CreateDevice(selected.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())),
                 "D3D12CreateDevice(WARP)");
}

using CompileFn = decltype(&D3DCompile);
CompileFn LoadCompiler(Module& module) {
    module.value = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module.value) { std::fprintf(stderr, "d3dcompiler_47.dll unavailable: %lu\n", GetLastError()); return nullptr; }
    FARPROC address = GetProcAddress(module.value, "D3DCompile");
    CompileFn function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "unexpected function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    if (!function) std::fprintf(stderr, "D3DCompile export unavailable\n");
    return function;
}

bool Compile(CompileFn compile, const char* source, const char* target, ComPtr<ID3DBlob>& result) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target,
                               D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                               0, result.put(), errors.put());
    if (errors.value) {
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        std::fputc('\n', stderr);
    }
    return Check(hr, target) && result.value;
}

bool WriteAll(HANDLE file, const void* data, DWORD bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (bytes) {
        DWORD written = 0;
        if (!WriteFile(file, p, bytes, &written, nullptr) || !written) return false;
        p += written;
        bytes -= written;
    }
    return true;
}

bool WritePpm(const wchar_t* path, const std::uint8_t* pixels, std::size_t offset, std::size_t pitch) {
    Handle file{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "cannot create output image (error %lu; existing files are not replaced)\n", GetLastError());
        return false;
    }
    static const char header[] = "P6\n256 256\n255\n";
    if (!WriteAll(file.value, header, static_cast<DWORD>(sizeof(header) - 1))) {
        std::fprintf(stderr, "incomplete output remains at %s\n", Utf8(path).c_str()); return false;
    }
    std::uint8_t row[kWidth * 3];
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const auto* p = pixels + offset + y * pitch + x * 4;
            row[x * 3] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2];
        }
        if (!WriteAll(file.value, row, sizeof(row))) {
            std::fprintf(stderr, "incomplete output remains at %s\n", Utf8(path).c_str()); return false;
        }
    }
    return true;
}

[[noreturn]] void AbortInFlight(ID3D12Device* device, const char* reason) {
    std::fprintf(stderr, "%s; device_removed_reason=0x%08lx\n", reason,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(device->GetDeviceRemovedReason())));
    std::fflush(nullptr);
    std::_Exit(4);  // Do not release resources which may still be in flight.
}

int Replay(const Frame& frame, const wchar_t* output) {
    ComPtr<IDXGIFactory4> factory;
    if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1")) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (!SelectDevice(factory.value, adapter, device)) return 1;
    DXGI_ADAPTER_DESC1 desc{};
    if (!Check(adapter->GetDesc1(&desc), "GetDesc1")) return 1;
    std::printf("adapter=%s vendor=0x%04lx device=0x%04lx software=%s\n", Utf8(desc.Description).c_str(),
                static_cast<unsigned long>(desc.VendorId), static_cast<unsigned long>(desc.DeviceId),
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "yes" : "no");

    Module compiler_module;
    CompileFn compiler = LoadCompiler(compiler_module);
    if (!compiler) return 1;
    // D3D8 XYZRHW positions are interpreted as integer-centered pixels.
    // Shift by +0.5 for DX12's half-integer sample centers; verify on T3 later.
    static const char vertex_source[] =
        "cbuffer Viewport : register(b0) { float2 size; };"
        "float4 main(float4 position : POSITION) : SV_Position {"
        "float w=rcp(position.w); float2 p=position.xy+0.5;"
        "return float4((p.x/size.x*2.0-1.0)*w,(1.0-p.y/size.y*2.0)*w,position.z*w,w); }";
    // D3D8 uses row-vector transforms. The adjustment moves projected points
    // by half a pixel in screen space before DX12's perspective divide.
    static const char camera_vertex_source[] =
        "cbuffer Transforms : register(b0) {"
        "row_major float4x4 world; row_major float4x4 view;"
        "row_major float4x4 projection; float2 size; };"
        "float4 main(float4 position : POSITION) : SV_Position {"
        "float4 p=mul(mul(mul(float4(position.xyz,1),world),view),projection);"
        "p.x+=p.w/size.x; p.y-=p.w/size.y; return p; }";
    static const char pixel_source[] =
        "cbuffer Diffuse : register(b1) { float4 color; };"
        "float4 main() : SV_Target { return color; }";
    ComPtr<ID3DBlob> vertex_shader, pixel_shader;
    if (!Compile(compiler, frame.camera ? camera_vertex_source : vertex_source, "vs_5_0", vertex_shader) ||
        !Compile(compiler, pixel_source, "ps_5_0", pixel_shader)) return 1;

    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> fence;
    if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateCommandQueue") ||
        !Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())),
               "CreateCommandAllocator") ||
        !Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence")) return 1;
    Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!event.value) { std::fprintf(stderr, "CreateEventW failed: %lu\n", GetLastError()); return 1; }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{}; rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (!Check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.put())), "CreateDescriptorHeap")) return 1;
    D3D12_HEAP_PROPERTIES target_heap{}; target_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    target_heap.CreationNodeMask = 1; target_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC target_desc{}; target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = kWidth; target_desc.Height = kHeight; target_desc.DepthOrArraySize = 1; target_desc.MipLevels = 1;
    target_desc.Format = kFormat; target_desc.SampleDesc.Count = 1; target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{}; clear.Format = kFormat;
    clear.Color[0] = static_cast<float>((frame.clear_argb >> 16) & 255) / 255.0f;
    clear.Color[1] = static_cast<float>((frame.clear_argb >> 8) & 255) / 255.0f;
    clear.Color[2] = static_cast<float>(frame.clear_argb & 255) / 255.0f;
    clear.Color[3] = static_cast<float>((frame.clear_argb >> 24) & 255) / 255.0f;
    ComPtr<ID3D12Resource> target;
    if (!Check(device->CreateCommittedResource(&target_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(target.put())), "CreateRenderTarget")) return 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.value, nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 row_size = 0, total_size = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, &rows, &row_size, &total_size);
    if (rows != kHeight || row_size != kWidth * 4 || footprint.Footprint.RowPitch < row_size ||
        !total_size || total_size > static_cast<UINT64>(SIZE_MAX) || footprint.Offset > total_size ||
        row_size > total_size - footprint.Offset ||
        UINT64(kHeight - 1) > (total_size - footprint.Offset - row_size) / footprint.Footprint.RowPitch) {
        std::fprintf(stderr, "unsupported readback footprint\n"); return 1;
    }
    D3D12_HEAP_PROPERTIES read_heap{}; read_heap.Type = D3D12_HEAP_TYPE_READBACK;
    read_heap.CreationNodeMask = 1; read_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC read_desc{}; read_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    read_desc.Width = total_size; read_desc.Height = 1; read_desc.DepthOrArraySize = 1; read_desc.MipLevels = 1;
    read_desc.SampleDesc.Count = 1; read_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (!Check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &read_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())), "CreateReadback")) return 1;

    D3D12_HEAP_PROPERTIES upload_heap{}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload_heap.CreationNodeMask = 1; upload_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC vertex_desc{}; vertex_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertex_desc.Width = sizeof(frame.vertices); vertex_desc.Height = 1;
    vertex_desc.DepthOrArraySize = 1; vertex_desc.MipLevels = 1; vertex_desc.SampleDesc.Count = 1;
    vertex_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> vertices;
    if (!Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vertices.put())), "CreateVertexUpload")) return 1;
    void* upload = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (!Check(vertices->Map(0, &no_read, &upload), "VertexUpload Map") || !upload) return 1;
    std::memcpy(upload, frame.vertices, sizeof(frame.vertices));
    const D3D12_RANGE written{0, sizeof(frame.vertices)};
    vertices->Unmap(0, &written);

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0; parameters[0].Constants.Num32BitValues = frame.camera ? 50 : 2;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 1; parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{}; root_desc.NumParameters = 2; root_desc.pParameters = parameters;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, root_errors;
    if (!Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            root_blob.put(), root_errors.put()), "SerializeRootSignature")) return 1;
    ComPtr<ID3D12RootSignature> root;
    if (!Check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(root.put())), "CreateRootSignature")) return 1;
    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.value;
    pso.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    pso.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    pso.InputLayout = {input, 1};
    for (auto& blend : pso.BlendState.RenderTarget) {
        blend.SrcBlend = D3D12_BLEND_ONE; blend.DestBlend = D3D12_BLEND_ZERO; blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE; pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; pso.DepthStencilState.StencilEnable = FALSE;
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
    const float viewport_size[2] = {static_cast<float>(kWidth), static_cast<float>(kHeight)};
    if (frame.camera) list->SetGraphicsRoot32BitConstants(0, 48, frame.matrices, 0);
    list->SetGraphicsRoot32BitConstants(0, 2, viewport_size, frame.camera ? 48 : 0);
    const std::uint32_t argb = frame.vertices[0].argb;
    const float diffuse[4] = {static_cast<float>((argb >> 16) & 255) / 255.0f,
                              static_cast<float>((argb >> 8) & 255) / 255.0f,
                              static_cast<float>(argb & 255) / 255.0f,
                              static_cast<float>((argb >> 24) & 255) / 255.0f};
    list->SetGraphicsRoot32BitConstants(1, 4, diffuse, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
    const D3D12_VERTEX_BUFFER_VIEW view{vertices->GetGPUVirtualAddress(), sizeof(frame.vertices), sizeof(Vertex)};
    list->IASetVertexBuffers(0, 1, &view);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.value; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.value;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = target.value;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!Check(list->Close(), "CloseCommandList")) return 1;
    ID3D12CommandList* lists[] = {list.value};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.value, 1))) AbortInFlight(device.value, "Queue::Signal failed");
    UINT64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) AbortInFlight(device.value, "Device removed before fence completion");
    if (completed < 1) {
        if (FAILED(fence->SetEventOnCompletion(1, event.value))) AbortInFlight(device.value, "Fence event failed");
        if (WaitForSingleObject(event.value, kFenceTimeoutMs) != WAIT_OBJECT_0) AbortInFlight(device.value, "Fence wait failed or timed out");
    }
    completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) AbortInFlight(device.value, "Device removed during fence wait");
    if (completed < 1) AbortInFlight(device.value, "Fence signalled without completion");
    if (!Check(device->GetDeviceRemovedReason(), "device status after fence")) return 1;
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    if (!Check(readback->Map(0, &read_range, &mapped), "Readback Map") || !mapped) return 1;
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    const std::size_t offset = static_cast<std::size_t>(footprint.Offset);
    const std::size_t pitch = footprint.Footprint.RowPitch;
    const std::uint8_t background[4] = {
        static_cast<std::uint8_t>(frame.clear_argb >> 16), static_cast<std::uint8_t>(frame.clear_argb >> 8),
        static_cast<std::uint8_t>(frame.clear_argb), static_cast<std::uint8_t>(frame.clear_argb >> 24)};
    std::uint64_t hash = UINT64_C(14695981039346656037);
    std::size_t changed_rgb = 0;
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const auto* pixel = pixels + offset + y * pitch + x * 4;
            if (std::memcmp(pixel, background, 3)) ++changed_rgb;
            for (unsigned channel = 0; channel < 4; ++channel) {
                hash ^= pixel[channel]; hash *= UINT64_C(1099511628211);
            }
        }
    }
    const bool output_ok = !output || (changed_rgb && WritePpm(output, pixels, offset, pitch));
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    if (!changed_rgb) {
        std::fprintf(stderr, "no visible pixels differ from clear; triangle may be clipped, degenerate or clear-colored\n");
        return 1;
    }
    if (!output_ok) return 1;
    std::printf("replay=%s viewport=%ux%u pitch=%u changed_pixels=%zu fnv1a64=%016llx\n",
                frame.camera ? "T3R_XYZ_V1" : "T3R_UP_V1", kWidth, kHeight,
                footprint.Footprint.RowPitch, changed_rgb, static_cast<unsigned long long>(hash));
    std::printf("pixel_centers=D3D8_integer_plus_0.5_to_DX12 (assumption); output=RGBA8\n");
    std::printf("result=PASS; no general D3D8 parity claimed\n");
    return 0;
}
#endif
}  // namespace

#ifndef T3RENDER_REPLAY_PARSER_TEST
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 4) {
        std::fwprintf(stderr, L"usage: d3d12_replay_up <input.txt> [--output <new-image.ppm>]\n"); return 2;
    }
    const wchar_t* output = nullptr;
    if (argc == 4) {
        if (std::wcscmp(argv[2], L"--output") || !argv[3][0]) {
            std::fwprintf(stderr, L"usage: d3d12_replay_up <input.txt> [--output <new-image.ppm>]\n"); return 2;
        }
        output = argv[3];
    }
    Frame frame;
    if (!ReadFrame(argv[1], frame)) return 2;
    return Replay(frame, output);
}
#endif
