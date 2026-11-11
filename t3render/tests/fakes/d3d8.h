#pragma once

// Only the signatures and fields exercised by capture.cpp. Not an ABI test.
#include <windows.h>

enum D3DTRANSFORMSTATETYPE { D3DTS_VIEW = 2, D3DTS_PROJECTION = 3 };
enum D3DRENDERSTATETYPE { D3DRS_FAKE = 0 };
enum D3DTEXTURESTAGESTATETYPE { D3DTSS_FAKE = 0 };
enum D3DPRIMITIVETYPE { D3DPT_FAKE = 0, D3DPT_TRIANGLELIST = 4 };
enum D3DFORMAT { D3DFMT_UNKNOWN = 0, D3DFMT_INDEX16 = 101 };
enum D3DRESOURCETYPE { D3DRTYPE_TEXTURE = 3, D3DRTYPE_VOLUMETEXTURE = 4, D3DRTYPE_CUBETEXTURE = 5 };

struct D3DMATRIX { float m[4][4]; };
struct D3DCOLORVALUE { float r, g, b, a; };
struct D3DVECTOR { float x, y, z; };
struct D3DVIEWPORT8 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    unsigned MultiSampleType, SwapEffect;
    BOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags, FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
struct D3DCAPS8 { DWORD MaxTextureBlendStages, MaxSimultaneousTextures, MaxStreams, MaxActiveLights; };
struct D3DSURFACE_DESC { D3DFORMAT Format; DWORD Usage, Pool, Width, Height; };
struct D3DVOLUME_DESC { D3DFORMAT Format; DWORD Usage, Pool, Width, Height, Depth; };
struct D3DVERTEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage, Pool, Size, FVF; };
struct D3DINDEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage, Pool, Size; };
struct D3DMATERIAL8 { D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive; float Power; };
struct D3DLIGHT8 {
    DWORD Type;
    D3DCOLORVALUE Diffuse, Specular, Ambient;
    D3DVECTOR Position, Direction;
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};

struct IDirect3DBaseTexture8 {
    virtual ULONG Release() = 0;
    virtual D3DRESOURCETYPE GetType() = 0;
    virtual DWORD GetLevelCount() = 0;
};
struct IDirect3DTexture8 : IDirect3DBaseTexture8 { virtual HRESULT GetLevelDesc(UINT, D3DSURFACE_DESC*) = 0; };
struct IDirect3DCubeTexture8 : IDirect3DBaseTexture8 { virtual HRESULT GetLevelDesc(UINT, D3DSURFACE_DESC*) = 0; };
struct IDirect3DVolumeTexture8 : IDirect3DBaseTexture8 { virtual HRESULT GetLevelDesc(UINT, D3DVOLUME_DESC*) = 0; };
struct IDirect3DVertexBuffer8 { virtual ULONG Release() = 0; virtual HRESULT GetDesc(D3DVERTEXBUFFER_DESC*) = 0; };
struct IDirect3DIndexBuffer8 { virtual ULONG Release() = 0; virtual HRESULT GetDesc(D3DINDEXBUFFER_DESC*) = 0; };
struct IDirect3DDevice8 {
    virtual HRESULT GetDeviceCaps(D3DCAPS8*) = 0;
    virtual HRESULT GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) = 0;
    virtual HRESULT GetViewport(D3DVIEWPORT8*) = 0;
    virtual HRESULT GetRenderState(D3DRENDERSTATETYPE, DWORD*) = 0;
    virtual HRESULT GetMaterial(D3DMATERIAL8*) = 0;
    virtual HRESULT GetLight(DWORD, D3DLIGHT8*) = 0;
    virtual HRESULT GetLightEnable(DWORD, BOOL*) = 0;
    virtual HRESULT GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD*) = 0;
    virtual HRESULT GetTexture(DWORD, IDirect3DBaseTexture8**) = 0;
    virtual HRESULT GetVertexShader(DWORD*) = 0;
    virtual HRESULT GetPixelShader(DWORD*) = 0;
    virtual HRESULT GetStreamSource(UINT, IDirect3DVertexBuffer8**, UINT*) = 0;
    virtual HRESULT GetIndices(IDirect3DIndexBuffer8**, UINT*) = 0;
};
