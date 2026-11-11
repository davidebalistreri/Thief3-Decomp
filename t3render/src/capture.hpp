#pragma once

#include <windows.h>
#include <d3d8.h>
#include <cstdint>

namespace t3render {

enum class DrawKind { Primitive, IndexedPrimitive, PrimitiveUP, IndexedPrimitiveUP };

// Parameters passed by the caller. UP pointers remain owned by the caller and
// are observed synchronously before forwarding; they are never retained.
struct DrawCall {
    DrawKind kind;
    D3DPRIMITIVETYPE primitive_type;
    UINT start_vertex, min_vertex_index, num_vertices, start_index, primitive_count, vertex_stride;
    D3DFORMAT index_format;
    const void* vertex_data = nullptr;
    const void* index_data = nullptr;
};

bool CaptureConfigured();
void CaptureDevice(IDirect3DDevice8*, const D3DPRESENT_PARAMETERS*);
// Called after final Release: the address is an identifier, not a live COM object.
void CaptureDeviceReleased(IDirect3DDevice8*);
// A nonzero token identifies an attempted operation in the selected frame.
// The matching result must be supplied after forwarding to the real device.
std::uint64_t CaptureDraw(IDirect3DDevice8*, const DrawCall&);
void CaptureDrawResult(IDirect3DDevice8*, std::uint64_t, HRESULT);
std::uint64_t CaptureClear(IDirect3DDevice8*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
void CaptureClearResult(IDirect3DDevice8*, std::uint64_t, HRESULT);
void CapturePresent(IDirect3DDevice8*, HRESULT);
void CaptureReset(IDirect3DDevice8*, const D3DPRESENT_PARAMETERS*, HRESULT);

}  // namespace t3render
