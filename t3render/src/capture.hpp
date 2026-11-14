#pragma once

#include <windows.h>
#include <d3d8.h>
#include <cstddef>
#include <cstdint>
#include <array>

#include "../ir/vertex_declaration_registry.hpp"
#include "buffer_diagnostics.hpp"

namespace t3render {

enum class DrawKind { Primitive, IndexedPrimitive, PrimitiveUP, IndexedPrimitiveUP };

// Optional bounded observations from the resource shadow. The callbacks copy
// exactly the requested range into the destination and return false if any
// byte or the owning resource is unknown. The provider is valid only during
// CaptureDraw; it must not call the D3D runtime or capture writer.
struct BufferObserver {
    void* context = nullptr;
    bool valid = false;
    bool (*read_vertex)(void*, IDirect3DDevice8*, IDirect3DVertexBuffer8*, std::size_t, void*, std::size_t) = nullptr;
    bool (*read_index)(void*, IDirect3DDevice8*, IDirect3DIndexBuffer8*, std::size_t, void*, std::size_t) = nullptr;
    const char* failure_status = nullptr; // optional static diagnostic strings
    const char* failure_cause = nullptr;
    unsigned failure_line = 0;
    buffers::UnlockFailure failure_unlock{};
};

// Optional getter results sampled outside the writer mutex. The caller owns
// the successful native +1 references through CaptureDraw and must prepare
// any COM identity lookups before calling it. A different owner fails closed.
struct BufferBindingSnapshot {
    IDirect3DDevice8* device = nullptr;
    HRESULT vertex_result = E_FAIL;
    IDirect3DVertexBuffer8* vertex = nullptr;
    UINT stride = 0;
    HRESULT index_result = E_FAIL;
    IDirect3DIndexBuffer8* index = nullptr;
    UINT base = 0;
};

// Candidate sampled outside the capture mutex. Its numeric match is not proof
// of shader identity or of bytes consumed by the driver.
struct ShaderSnapshot {
    HRESULT getter_result = E_FAIL;
    DWORD value = 0;
    ir::ShaderObservation declaration;
    // Raw IEEE-754 bits sampled from the runtime before the draw. The getter
    // does not prove what an unobserved alias or the GPU actually used.
    bool constants_requested = false;
    HRESULT constants_result = E_FAIL;
    HRESULT constants_verify_result = E_FAIL;
    bool constants_match = false;
    std::uint32_t constants_count = 0;
    std::array<std::uint32_t, 96u * 4u> constant_bits{};
};

// Parameters passed by the caller. UP pointers remain owned by the caller and
// are observed synchronously before forwarding; they are never retained.
struct DrawCall {
    DrawKind kind;
    D3DPRIMITIVETYPE primitive_type;
    UINT start_vertex, min_vertex_index, num_vertices, start_index, primitive_count, vertex_stride;
    D3DFORMAT index_format;
    const void* vertex_data = nullptr;
    const void* index_data = nullptr;
    const BufferObserver* buffer_observer = nullptr;
    const BufferBindingSnapshot* buffer_bindings = nullptr;
    const ShaderSnapshot* shader_snapshot = nullptr;
};

bool CaptureConfigured();
// Marks the opt-in experimental facade before the first frame is opened.
void CaptureExperimentalFacade();
// Advisory only: a concurrent Present may change the active window.
bool CaptureWantsDraw(IDirect3DDevice8*);
// 0 means unavailable caps or a disabled constant observation.
std::uint32_t CaptureVertexConstantCount(IDirect3DDevice8*);
void CaptureDevice(IDirect3DDevice8*, const D3DPRESENT_PARAMETERS*);
// Called after final Release: the address is an identifier, not a live COM object.
void CaptureDeviceReleased(IDirect3DDevice8*);
// Stop a partial trace after an unrecorded nested device operation; a null
// device abandons the current capture without needing its identity.
void CaptureAbandonDevice(IDirect3DDevice8*);
// A nonzero token identifies an attempted operation in the selected frame.
// The matching result must be supplied after forwarding to the real device.
std::uint64_t CaptureDraw(IDirect3DDevice8*, const DrawCall&);
void CaptureDrawResult(IDirect3DDevice8*, std::uint64_t, HRESULT);
// The snapshot was taken before the driver draw; stable is true only when the
// whole observed draw window completed without an intercepted hazard.
void CaptureBufferedDrawResult(IDirect3DDevice8*, std::uint64_t, HRESULT, bool stable);
// buffer_stable is -1 when no buffer observation was made.
void CaptureShaderDrawResult(IDirect3DDevice8*, std::uint64_t, HRESULT, int buffer_stable, bool shader_stable);
std::uint64_t CaptureClear(IDirect3DDevice8*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
void CaptureClearResult(IDirect3DDevice8*, std::uint64_t, HRESULT);
void CapturePresent(IDirect3DDevice8*, HRESULT);
void CaptureReset(IDirect3DDevice8*, const D3DPRESENT_PARAMETERS*, HRESULT);

}  // namespace t3render
