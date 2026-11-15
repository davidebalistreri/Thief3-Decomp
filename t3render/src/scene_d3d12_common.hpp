#pragma once

// Shared D3D12 resource helpers for the separate SceneIR render paths.
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

namespace t3render::dx12 {
using scene::Texture;
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

inline bool Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s: HRESULT=0x%08lx\n", operation,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}

using CompileFn = decltype(&D3DCompile);
inline bool Compile(CompileFn compile, const char* source, const char* profile, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
                               D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                               blob.put(), errors.put());
    if (errors.p) { std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr); std::fputc('\n', stderr); }
    return Check(hr, profile) && blob.p;
}

inline bool SelectDevice(ComPtr<ID3D12Device>& device) {
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

inline D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1; return heap;
}
inline D3D12_RESOURCE_DESC Buffer(UINT64 size) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return desc;
}
inline D3D12_RESOURCE_DESC Image(UINT width, UINT height, UINT16 mips, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = mips;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags = flags;
    return desc;
}
inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

inline bool UploadTexture(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Texture& texture, GpuTexture& gpu,
                          DXGI_FORMAT resource_format = kColor) {
    const auto desc = Image(texture.mips[0].width, texture.mips[0].height, static_cast<UINT16>(texture.mips.size()),
                            resource_format, D3D12_RESOURCE_FLAG_NONE);
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

inline bool WriteAll(HANDLE file, const void* data, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size) { DWORD done = 0; if (!WriteFile(file, bytes, size, &done, nullptr) || !done) return false; bytes += done; size -= done; }
    return true;
}
inline bool WriteImage(const wchar_t* path, const std::uint8_t* bytes, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
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

[[noreturn]] inline void AbortInFlight(const char* message) {
    std::fprintf(stderr, "%s; GPU resources retained until process exit\n", message);
    std::fflush(nullptr); std::_Exit(4);
}

} // namespace t3render::dx12
