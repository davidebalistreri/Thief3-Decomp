#pragma once
#include "windows.h"
typedef int D3DDEVTYPE; typedef int D3DFORMAT; typedef int D3DPRIMITIVETYPE;
typedef int D3DPOOL; typedef int D3DRESOURCETYPE; typedef int D3DSTATEBLOCKTYPE;
typedef DWORD D3DCOLOR;
typedef struct { int32_t x1, y1, x2, y2; } D3DRECT;
#define D3DERR_INVALIDCALL ((HRESULT)-7)
#define D3DPOOL_MANAGED 1
#define D3DFMT_INDEX16 101
#define D3DRTYPE_VERTEXBUFFER 6
#define D3DRTYPE_INDEXBUFFER 7
#define D3DUSAGE_WRITEONLY 8
#define D3DSPD_IUNKNOWN 1
#define D3DLOCK_READONLY 0x10
#define D3DLOCK_DISCARD 0x2000
#define D3DLOCK_NOOVERWRITE 0x1000
#define D3DLOCK_NOSYSLOCK 0x0800
typedef struct { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; DWORD FVF; } D3DVERTEXBUFFER_DESC;
typedef struct { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; } D3DINDEXBUFFER_DESC;
typedef struct { int marker; } D3DPRESENT_PARAMETERS;
typedef struct IDirect3D8 IDirect3D8; typedef struct IDirect3DDevice8 IDirect3DDevice8;
typedef struct IDirect3DVertexBuffer8 IDirect3DVertexBuffer8;
typedef struct IDirect3DIndexBuffer8 IDirect3DIndexBuffer8;
#ifdef __cplusplus
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
#endif
#ifdef __cplusplus
extern "C" {
#endif
IDirect3D8* WINAPI Direct3DCreate8(UINT);
#ifdef __cplusplus
}
#endif
struct IDirect3D8Vtbl {
 HRESULT (*QueryInterface)(IDirect3D8*,REFIID,void**); ULONG (*AddRef)(IDirect3D8*); ULONG (*Release)(IDirect3D8*);
 HRESULT (*CreateDevice)(IDirect3D8*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice8**);
};
struct IDirect3DDevice8Vtbl {
 HRESULT (*QueryInterface)(IDirect3DDevice8*,REFIID,void**); ULONG (*AddRef)(IDirect3DDevice8*); ULONG (*Release)(IDirect3DDevice8*);
 HRESULT (*Reset)(IDirect3DDevice8*,D3DPRESENT_PARAMETERS*);
 HRESULT (*Present)(IDirect3DDevice8*,const RECT*,const RECT*,HWND,const RGNDATA*);
 HRESULT (*Clear)(IDirect3DDevice8*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
 HRESULT (*DrawPrimitive)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,UINT);
 HRESULT (*DrawIndexedPrimitive)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
 HRESULT (*DrawPrimitiveUP)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
 HRESULT (*DrawIndexedPrimitiveUP)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
 HRESULT (*CreateVertexBuffer)(IDirect3DDevice8*,UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer8**);
 HRESULT (*CreateIndexBuffer)(IDirect3DDevice8*,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DIndexBuffer8**);
 HRESULT (*SetStreamSource)(IDirect3DDevice8*,UINT,IDirect3DVertexBuffer8*,UINT);
 HRESULT (*GetStreamSource)(IDirect3DDevice8*,UINT,IDirect3DVertexBuffer8**,UINT*);
 HRESULT (*SetIndices)(IDirect3DDevice8*,IDirect3DIndexBuffer8*,UINT);
 HRESULT (*GetIndices)(IDirect3DDevice8*,IDirect3DIndexBuffer8**,UINT*);
 HRESULT (*ProcessVertices)(IDirect3DDevice8*,UINT,UINT,UINT,IDirect3DVertexBuffer8*,DWORD);
 HRESULT (*BeginStateBlock)(IDirect3DDevice8*);
 HRESULT (*EndStateBlock)(IDirect3DDevice8*,DWORD*);
 HRESULT (*ApplyStateBlock)(IDirect3DDevice8*,DWORD);
 HRESULT (*CaptureStateBlock)(IDirect3DDevice8*,DWORD);
 HRESULT (*DeleteStateBlock)(IDirect3DDevice8*,DWORD);
 HRESULT (*CreateStateBlock)(IDirect3DDevice8*,D3DSTATEBLOCKTYPE,DWORD*);
};
struct IDirect3D8 {
 struct IDirect3D8Vtbl* lpVtbl; int refs;
#ifdef __cplusplus
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**);
 ULONG STDMETHODCALLTYPE Release();
 HRESULT STDMETHODCALLTYPE CreateDevice(UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice8**);
#endif
};
struct IDirect3DDevice8 {
 struct IDirect3DDevice8Vtbl* lpVtbl; int refs;
#ifdef __cplusplus
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**);
 ULONG STDMETHODCALLTYPE Release();
 HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS*);
 HRESULT STDMETHODCALLTYPE Present(const RECT*,const RECT*,HWND,const RGNDATA*);
 HRESULT STDMETHODCALLTYPE Clear(DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
 HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE,UINT,UINT);
 HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
 HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE,UINT,const void*,UINT);
 HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
 HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer8**);
 HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DIndexBuffer8**);
 HRESULT STDMETHODCALLTYPE SetStreamSource(UINT,IDirect3DVertexBuffer8*,UINT);
 HRESULT STDMETHODCALLTYPE GetStreamSource(UINT,IDirect3DVertexBuffer8**,UINT*);
 HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8*,UINT);
 HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8**,UINT*);
 HRESULT STDMETHODCALLTYPE ProcessVertices(UINT,UINT,UINT,IDirect3DVertexBuffer8*,DWORD);
 HRESULT STDMETHODCALLTYPE BeginStateBlock();
 HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD*);
 HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD);
 HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD);
 HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD);
 HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE,DWORD*);
#endif
};
