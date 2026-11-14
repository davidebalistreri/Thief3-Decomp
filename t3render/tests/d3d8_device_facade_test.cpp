#include "d3d8_device_facade.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

// Controlled failure affects the next facade nothrow allocation only.
static std::atomic<bool> fail_next_nothrow{false};
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (fail_next_nothrow.exchange(false)) return nullptr;
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

namespace {
constexpr GUID kUnknownIid{99, 0, 0, {}};
struct NativeDevice final : IDirect3DDevice8 {
    ULONG refs = 0;
    unsigned adds = 0, releases = 0, queries = 0, get_parent = 0, draw_calls = 0, transforms = 0;
    IDirect3D8* parent = nullptr;
    IDirect3DBaseTexture8* texture = nullptr;
    HRESULT get_parent_result = S_OK;
    bool null_parent = false, failed_nonnull = false;
    UINT draw_start = 0, draw_count = 0;
    D3DPRIMITIVETYPE draw_type = 0;
    const D3DMATRIX* matrix = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
        ++queries; if (!output) return E_POINTER; *output = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++adds; return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; SetLastError(222); return --refs; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** output) override {
        ++get_parent; SetLastError(111);
        if (FAILED(get_parent_result)) {
            if (failed_nonnull) *output = reinterpret_cast<IDirect3D8*>(1);
            return get_parent_result;
        }
        if (null_parent) { *output = nullptr; return S_OK; }
        parent->AddRef(); *output = parent; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override {
        ++draw_calls; draw_type = type; draw_start = start; draw_count = count; return 17;
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX* value) override {
        ++transforms; matrix = value; return 18;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD, IDirect3DBaseTexture8* value) override { texture = value; return 19; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD, IDirect3DBaseTexture8** out) override { *out = texture; return 20; }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {  return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {  return 0; }
    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD Bytes) override { (void)Bytes; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8 * pCaps) override { (void)pCaps; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE * pMode) override { (void)pMode; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS * pParameters) override { (void)pParameters; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 * pCursorBitmap) override { (void)XHotSpot; (void)YHotSpot; (void)pCursorBitmap; return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace,DWORD Flags) override { (void)XScreenSpace; (void)YScreenSpace; (void)Flags;  }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { (void)bShow; return 0; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS * pPresentationParameters, IDirect3DSwapChain8 ** pSwapChain) override { (void)pPresentationParameters; (void)pSwapChain; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS * pPresentationParameters) override { (void)pPresentationParameters; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) override { (void)src_rect; (void)dst_rect; (void)dst_window_override; (void)dirty_region; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface8 ** ppBackBuffer) override { (void)BackBuffer; (void)Type; (void)ppBackBuffer; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS * pRasterStatus) override { (void)pRasterStatus; return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3DGAMMARAMP *ramp) override { (void)flags; (void)ramp;  }
    void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP * pRamp) override { (void)pRamp;  }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture8 ** ppTexture) override { (void)Width; (void)Height; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)ppTexture; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture8 ** ppVolumeTexture) override { (void)Width; (void)Height; (void)Depth; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)ppVolumeTexture; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture8 ** ppCubeTexture) override { (void)EdgeLength; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)ppCubeTexture; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer8 ** ppVertexBuffer) override { (void)Length; (void)Usage; (void)FVF; (void)Pool; (void)ppVertexBuffer; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer8 ** ppIndexBuffer) override { (void)Length; (void)Usage; (void)Format; (void)Pool; (void)ppIndexBuffer; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,BOOL Lockable,IDirect3DSurface8 ** ppSurface) override { (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)Lockable; (void)ppSurface; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,IDirect3DSurface8 ** ppSurface) override { (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)ppSurface; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT Width,UINT Height,D3DFORMAT Format,IDirect3DSurface8 ** ppSurface) override { (void)Width; (void)Height; (void)Format; (void)ppSurface; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8 *src_surface, const RECT *src_rects, UINT rect_count, IDirect3DSurface8 *dst_surface, const POINT *dst_points) override { (void)src_surface; (void)src_rects; (void)rect_count; (void)dst_surface; (void)dst_points; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8 * pSourceTexture,IDirect3DBaseTexture8 * pDestinationTexture) override { (void)pSourceTexture; (void)pDestinationTexture; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8 * pDestSurface) override { (void)pDestSurface; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8 * pRenderTarget,IDirect3DSurface8 * pNewZStencil) override { (void)pRenderTarget; (void)pNewZStencil; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8 ** ppRenderTarget) override { (void)ppRenderTarget; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8 ** ppZStencilSurface) override { (void)ppZStencilSurface; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginScene() override {  return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndScene() override {  return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override { (void)rect_count; (void)rects; (void)flags; (void)color; (void)z; (void)stencil; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,D3DMATRIX * pMatrix) override { (void)State; (void)pMatrix; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override { (void)state; (void)matrix; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8 *viewport) override { (void)viewport; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8 * pViewport) override { (void)pViewport; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8 *material) override { (void)material; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8 *pMaterial) override { (void)pMaterial; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT8 *light) override { (void)index; (void)light; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index,D3DLIGHT8 * pLight) override { (void)Index; (void)pLight; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index,BOOL Enable) override { (void)Index; (void)Enable; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index,BOOL * pEnable) override { (void)Index; (void)pEnable; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override { (void)index; (void)plane; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index,float * pPlane) override { (void)Index; (void)pPlane; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,DWORD Value) override { (void)State; (void)Value; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,DWORD * pValue) override { (void)State; (void)pValue; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {  return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD * pToken) override { (void)pToken; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD Token) override { (void)Token; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD Token) override { (void)Token; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD Token) override { (void)Token; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type,DWORD * pToken) override { (void)Type; (void)pToken; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8 *clip_status) override { (void)clip_status; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8 * pClipStatus) override { (void)pClipStatus; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD * pValue) override { (void)Stage; (void)Type; (void)pValue; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value) override { (void)Stage; (void)Type; (void)Value; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD * pNumPasses) override { (void)pNumPasses; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD DevInfoID,void * pDevInfoStruct,DWORD DevInfoStructSize) override { (void)DevInfoID; (void)pDevInfoStruct; (void)DevInfoStructSize; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override { (void)palette_idx; (void)entries; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,PALETTEENTRY * pEntries) override { (void)PaletteNumber; (void)pEntries; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { (void)PaletteNumber; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT * PaletteNumber) override { (void)PaletteNumber; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT minIndex,UINT NumVertices,UINT startIndex,UINT primCount) override { (void)PrimitiveType; (void)minIndex; (void)NumVertices; (void)startIndex; (void)primCount; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) override { (void)primitive_type; (void)primitive_count; (void)data; (void)stride; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, D3DFORMAT index_format, const void *data, UINT stride) override { (void)primitive_type; (void)min_vertex_idx; (void)vertex_count; (void)primitive_count; (void)index_data; (void)index_format; (void)data; (void)stride; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer8 * pDestBuffer,DWORD Flags) override { (void)SrcStartIndex; (void)DestIndex; (void)VertexCount; (void)pDestBuffer; (void)Flags; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *declaration, const DWORD *byte_code, DWORD *shader, DWORD usage) override { (void)declaration; (void)byte_code; (void)shader; (void)usage; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD Handle) override { (void)Handle; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD * pHandle) override { (void)pHandle; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD Handle) override { (void)Handle; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg_idx, const void *data, DWORD count) override { (void)reg_idx; (void)data; (void)count; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD Register,void * pConstantData,DWORD ConstantCount) override { (void)Register; (void)pConstantData; (void)ConstantCount; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD Handle,void * pData,DWORD * pSizeOfData) override { (void)Handle; (void)pData; (void)pSizeOfData; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD Handle,void * pData,DWORD * pSizeOfData) override { (void)Handle; (void)pData; (void)pSizeOfData; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer8 * pStreamData,UINT Stride) override { (void)StreamNumber; (void)pStreamData; (void)Stride; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer8 ** ppStreamData,UINT * pStride) override { (void)StreamNumber; (void)ppStreamData; (void)pStride; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8 * pIndexData,UINT BaseVertexIndex) override { (void)pIndexData; (void)BaseVertexIndex; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8 ** ppIndexData,UINT * pBaseVertexIndex) override { (void)ppIndexData; (void)pBaseVertexIndex; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, DWORD *shader) override { (void)byte_code; (void)shader; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD Handle) override { (void)Handle; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD * pHandle) override { (void)pHandle; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD Handle) override { (void)Handle; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg_idx, const void *data, DWORD count) override { (void)reg_idx; (void)data; (void)count; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD Register,void * pConstantData,DWORD ConstantCount) override { (void)Register; (void)pConstantData; (void)ConstantCount; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD Handle,void * pData,DWORD * pSizeOfData) override { (void)Handle; (void)pData; (void)pSizeOfData; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segment_count, const D3DRECTPATCH_INFO *patch_info) override { (void)handle; (void)segment_count; (void)patch_info; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segment_count, const D3DTRIPATCH_INFO *patch_info) override { (void)handle; (void)segment_count; (void)patch_info; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { (void)Handle; return E_NOTIMPL; }
};

struct NativeFactory final : IDirect3D8 {
    ULONG refs = 1;
    unsigned adds = 0, releases = 0, queries = 0, creates = 0;
    NativeDevice device;
    HRESULT create_result = S_OK;
    bool failed_nonnull = false, fail_identity = false;
    NativeFactory* identity = nullptr;
    ULONG created_adapter = 0;
    DWORD created_behavior = 0;
    D3DPRESENT_PARAMETERS* created_presentation = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
        ++queries;
        if (!output) return E_POINTER;
        *output = nullptr;
        if (fail_identity) return E_FAIL;
        auto* canonical = identity ? identity : this;
        canonical->AddRef(); *output = static_cast<IDirect3D8*>(canonical); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++adds; return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; SetLastError(333); return --refs; }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE, HWND, DWORD behavior,
                                            D3DPRESENT_PARAMETERS* presentation, IDirect3DDevice8** output) override {
        ++creates; created_adapter = adapter; created_behavior = behavior; created_presentation = presentation;
        SetLastError(111);
        if (FAILED(create_result)) {
            if (failed_nonnull) *output = reinterpret_cast<IDirect3DDevice8*>(1);
            return create_result;
        }
        assert(device.refs == 0);
        device.refs = 1; device.parent = this; *output = &device; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void * pInitializeFunction) override { (void)pInitializeFunction; return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetAdapterCount() override {  return 0; }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 * pIdentifier) override { (void)Adapter; (void)Flags; (void)pIdentifier; return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter) override { (void)Adapter; return 0; }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE * pMode) override { (void)Adapter; (void)Mode; (void)pMode; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE * pMode) override { (void)Adapter; (void)pMode; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) override { (void)Adapter; (void)CheckType; (void)DisplayFormat; (void)BackBufferFormat; (void)Windowed; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { (void)Adapter; (void)DeviceType; (void)AdapterFormat; (void)Usage; (void)RType; (void)CheckFormat; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) override { (void)Adapter; (void)DeviceType; (void)SurfaceFormat; (void)Windowed; (void)MultiSampleType; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { (void)Adapter; (void)DeviceType; (void)AdapterFormat; (void)RenderTargetFormat; (void)DepthStencilFormat; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 * pCaps) override { (void)Adapter; (void)DeviceType; (void)pCaps; return E_NOTIMPL; }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { (void)Adapter; return nullptr; }
};

void FactoryCreationFailureDoesNotConsumeNative() {
    NativeFactory native;
    fail_next_nothrow = true;
    SetLastError(44);
    assert(!t3render::fixture::CreateDeviceFacadeFactory(&native));
    assert(native.refs == 1 && native.adds == 0 && native.releases == 0 && GetLastError() == 44);
    assert(!t3render::fixture::CreateDeviceFacadeFactory(nullptr));
}

void DeviceAllocationFailureCleansUpNative() {
    NativeFactory native;
    IDirect3D8* factory = t3render::fixture::CreateDeviceFacadeFactory(&native);
    assert(factory);
    D3DPRESENT_PARAMETERS params{};
    IDirect3DDevice8* output = reinterpret_cast<IDirect3DDevice8*>(1);
    fail_next_nothrow = true;
    assert(factory->CreateDevice(4, 5, nullptr, 6, &params, &output) == E_OUTOFMEMORY);
    assert(!output && native.creates == 1 && native.created_adapter == 4 && native.created_behavior == 6 &&
           native.created_presentation == &params);
    assert(native.device.refs == 0 && native.device.releases == 1 && GetLastError() == 111);
    assert(factory->Release() == 0 && native.refs == 0 && native.releases == 1);
}

void FailedNativeDeviceAndNullOutputs() {
    NativeFactory native;
    IDirect3D8* factory = t3render::fixture::CreateDeviceFacadeFactory(&native);
    assert(factory);
    D3DPRESENT_PARAMETERS params{};
    assert(factory->CreateDevice(0, 0, nullptr, 0, &params, nullptr) == E_POINTER && native.creates == 0);
    native.create_result = E_FAIL;
    IDirect3DDevice8* output = reinterpret_cast<IDirect3DDevice8*>(1);
    assert(factory->CreateDevice(0, 0, nullptr, 0, &params, &output) == E_FAIL && !output);
    native.failed_nonnull = true;
    assert(factory->CreateDevice(0, 0, nullptr, 0, &params, &output) == E_FAIL && !output);
    assert(native.device.releases == 0 && GetLastError() == 111);
    assert(factory->Release() == 0 && native.releases == 1);
}

void IdentityLifetimeAndForwarding() {
    NativeFactory native;
    IDirect3D8* factory = t3render::fixture::CreateDeviceFacadeFactory(&native);
    assert(factory);
    void* factory_unknown = nullptr;
    assert(factory->QueryInterface(IID_IUnknown, &factory_unknown) == S_OK && factory_unknown == factory);
    assert(static_cast<IUnknown*>(factory_unknown)->Release() == 1);
    void* unsupported = reinterpret_cast<void*>(1);
    assert(factory->QueryInterface(kUnknownIid, &unsupported) == E_NOINTERFACE && !unsupported && !native.queries);
    D3DPRESENT_PARAMETERS params{};
    IDirect3DDevice8* device = nullptr;
    assert(factory->CreateDevice(0, 0, nullptr, 0, &params, &device) == S_OK && device);
    assert(device != &native.device && native.device.refs == 1 && GetLastError() == 111);
    void* device_unknown = nullptr;
    assert(device->QueryInterface(IID_IUnknown, &device_unknown) == S_OK && device_unknown == device);
    void* device_again = nullptr;
    assert(static_cast<IUnknown*>(device_unknown)->QueryInterface(IID_IDirect3DDevice8, &device_again) == S_OK && device_again == device);
    unsupported = reinterpret_cast<void*>(1);
    assert(device->QueryInterface(kUnknownIid, &unsupported) == E_NOINTERFACE && !unsupported && !native.device.queries);
    assert(device->QueryInterface(IID_IUnknown, nullptr) == E_POINTER);
    assert(static_cast<IDirect3DDevice8*>(device_again)->Release() == 2);
    assert(static_cast<IUnknown*>(device_unknown)->Release() == 1);

    D3DMATRIX matrix{};
    assert(device->SetTransform(7, &matrix) == 18 && native.device.transforms == 1 && native.device.matrix == &matrix);
    assert(device->DrawPrimitive(5, 12, 3) == 17 && native.device.draw_calls == 1 &&
           native.device.draw_type == 5 && native.device.draw_start == 12 && native.device.draw_count == 3);
    auto* raw = reinterpret_cast<IDirect3DBaseTexture8*>(0x1234);
    assert(device->SetTexture(1, raw) == 19 && native.device.texture == raw);
    IDirect3DBaseTexture8* returned = nullptr;
    assert(device->GetTexture(1, &returned) == 20 && returned == raw);

    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (unsigned n = 0; n < 3000; ++n) { assert(device->AddRef() >= 2); device->Release(); }
    });
    for (auto& thread : threads) thread.join();
    // The device holds the factory facade after its caller releases it.
    assert(factory->Release() == 1 && native.refs == 1 && native.releases == 0);
    IDirect3D8* parent = nullptr;
    assert(device->GetDirect3D(&parent) == S_OK && parent == factory && GetLastError() == 111);
    assert(native.device.get_parent == 1 && native.refs == 1 && native.adds == 3 && native.releases == 3);
    assert(parent->Release() == 1);
    NativeFactory alias;
    alias.identity = &native;
    native.device.parent = &alias;
    assert(device->GetDirect3D(&parent) == S_OK && parent == factory && GetLastError() == 111);
    assert(parent->Release() == 1 && alias.refs == 1 && native.refs == 1);
    NativeFactory unrelated;
    native.device.parent = &unrelated;
    parent = reinterpret_cast<IDirect3D8*>(1);
    assert(device->GetDirect3D(&parent) == E_FAIL && !parent && GetLastError() == 111);
    assert(unrelated.refs == 1 && native.refs == 1);
    native.device.parent = &native;
    native.fail_identity = true;
    assert(device->GetDirect3D(&parent) == E_FAIL && !parent && GetLastError() == 111);
    assert(native.refs == 1);
    native.fail_identity = false;
    assert(device->GetDirect3D(nullptr) == E_POINTER);
    native.device.get_parent_result = E_FAIL;
    parent = reinterpret_cast<IDirect3D8*>(1);
    assert(device->GetDirect3D(&parent) == E_FAIL && !parent && GetLastError() == 111);
    native.device.failed_nonnull = true;
    assert(device->GetDirect3D(&parent) == E_FAIL && !parent && GetLastError() == 111);
    native.device.get_parent_result = S_OK; native.device.null_parent = true;
    assert(device->GetDirect3D(&parent) == E_FAIL && !parent && GetLastError() == 111);
    SetLastError(77);
    assert(device->Release() == 0 && GetLastError() == 222);
    assert(native.device.refs == 0 && native.device.releases == 1);
    assert(native.refs == 0 && native.adds == native.releases - 1);
}
} // namespace

int main() {
    FactoryCreationFailureDoesNotConsumeNative();
    DeviceAllocationFailureCleansUpNative();
    FailedNativeDeviceAndNullOutputs();
    IdentityLifetimeAndForwarding();
}
