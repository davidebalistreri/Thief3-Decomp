#pragma once
#include "../buffer_proxy_fakes/d3d8.h"

inline constexpr IID IID_IDirect3DSurface8{6};
inline constexpr IID IID_IDirect3DBaseTexture8{7};
inline constexpr IID IID_IDirect3DTexture8{8};
inline constexpr IID IID_IDirect3DCubeTexture8{9};
inline constexpr IID IID_IDirect3DVolumeTexture8{10};
inline constexpr IID IID_IDirect3DVolume8{11};
inline constexpr IID IID_IDirect3DSwapChain8{12};

struct D3DSURFACE_DESC { UINT Width, Height; };
struct D3DLOCKED_RECT { int Pitch; void* pBits; };
struct D3DVOLUME_DESC { UINT Width, Height, Depth; };
struct D3DLOCKED_BOX { int RowPitch, SlicePitch; void* pBits; };
struct D3DBOX { UINT Left, Top, Right, Bottom, Front, Back; };
enum D3DCUBEMAP_FACES { D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1 };
enum D3DBACKBUFFER_TYPE { D3DBACKBUFFER_TYPE_MONO = 0 };
inline constexpr D3DRESOURCETYPE D3DRTYPE_TEXTURE = static_cast<D3DRESOURCETYPE>(3);
inline constexpr D3DRESOURCETYPE D3DRTYPE_CUBETEXTURE = static_cast<D3DRESOURCETYPE>(5);
inline constexpr D3DRESOURCETYPE D3DRTYPE_VOLUMETEXTURE = static_cast<D3DRESOURCETYPE>(4);

struct IDirect3DVolume8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX*, const D3DBOX*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockBox() = 0;
};

struct IDirect3DSurface8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT*, const RECT*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockRect() = 0;
};
struct IDirect3DSwapChain8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Present(const RECT*, const RECT*, HWND, const RGNDATA*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8**) = 0;
};
struct IDirect3DBaseTexture8 : IDirect3DResource8 {
    virtual DWORD STDMETHODCALLTYPE SetLOD(DWORD) = 0;
    virtual DWORD STDMETHODCALLTYPE GetLOD() = 0;
    virtual DWORD STDMETHODCALLTYPE GetLevelCount() = 0;
};
struct IDirect3DTexture8 : IDirect3DBaseTexture8 {
    virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT, D3DSURFACE_DESC*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT, IDirect3DSurface8**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockRect(UINT, D3DLOCKED_RECT*, const RECT*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockRect(UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) = 0;
};
struct IDirect3DCubeTexture8 : IDirect3DBaseTexture8 {
    virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT, D3DSURFACE_DESC*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES, UINT, IDirect3DSurface8**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) = 0;
};
struct IDirect3DVolumeTexture8 : IDirect3DBaseTexture8 {
    virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT, D3DVOLUME_DESC*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT, IDirect3DVolume8**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockBox(UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockBox(UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX*) = 0;
};
