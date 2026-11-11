#pragma once

#include <windows.h>
#include <d3d8.h>

namespace t3render {

enum class DrawKind { Primitive, IndexedPrimitive, PrimitiveUP, IndexedPrimitiveUP };

// Parameters passed by the caller. UP draws are observed before the call;
// their transient vertex and index contents are not included in this contract.
struct DrawCall {
    DrawKind kind;
    D3DPRIMITIVETYPE primitive_type;
    UINT start_vertex, min_vertex_index, num_vertices, start_index, primitive_count, vertex_stride;
    D3DFORMAT index_format;
};

bool CaptureConfigured();
void CaptureDevice(IDirect3DDevice8*, const D3DPRESENT_PARAMETERS*);
// Called after final Release: the address is an identifier, not a live COM object.
void CaptureDeviceReleased(IDirect3DDevice8*);
void CaptureDraw(IDirect3DDevice8*, const DrawCall&);
void CapturePresent(IDirect3DDevice8*, HRESULT);
void CaptureReset(IDirect3DDevice8*, const D3DPRESENT_PARAMETERS*, HRESULT);

}  // namespace t3render
