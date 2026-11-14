#pragma once

#include <windows.h>

inline constexpr IID IID_IDirect3DResource8{2};
inline constexpr IID IID_IDirect3DVertexBuffer8{3};
inline constexpr IID IID_IDirect3DIndexBuffer8{4};
inline constexpr IID IID_IDirect3DDevice8{5};
inline constexpr HRESULT D3DERR_INVALIDCALL = -5;
inline constexpr HRESULT D3DERR_NOTFOUND = -7;
inline constexpr DWORD D3DLOCK_READONLY = 0x10;
inline constexpr DWORD D3DLOCK_DISCARD = 0x2000;
inline constexpr DWORD D3DLOCK_NOOVERWRITE = 0x1000;
inline constexpr DWORD D3DLOCK_NOSYSLOCK = 0x0800;
inline constexpr DWORD D3DUSAGE_WRITEONLY = 8;
inline constexpr DWORD D3DSPD_IUNKNOWN = 1;

enum D3DRESOURCETYPE { D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7 };
enum D3DFORMAT { D3DFMT_UNKNOWN = 0, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102 };
enum D3DPOOL { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3 };
struct D3DVERTEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; DWORD FVF; };
struct D3DINDEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; };

struct IDirect3DDevice8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;

struct IDirect3DResource8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) = 0;
    virtual DWORD STDMETHODCALLTYPE SetPriority(DWORD) = 0;
    virtual DWORD STDMETHODCALLTYPE GetPriority() = 0;
    virtual void STDMETHODCALLTYPE PreLoad() = 0;
    virtual D3DRESOURCETYPE STDMETHODCALLTYPE GetType() = 0;
};
struct IDirect3DVertexBuffer8 : IDirect3DResource8 {
    virtual HRESULT STDMETHODCALLTYPE Lock(UINT, UINT, BYTE**, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unlock() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC*) = 0;
};
struct IDirect3DIndexBuffer8 : IDirect3DResource8 {
    virtual HRESULT STDMETHODCALLTYPE Lock(UINT, UINT, BYTE**, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unlock() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC*) = 0;
};
struct IDirect3DDevice8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStreamSource(UINT, IDirect3DVertexBuffer8*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamSource(UINT, IDirect3DVertexBuffer8**, UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8**, UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) = 0;
};
