#pragma once
#include "windows.h"
typedef int D3DDEVTYPE; typedef int D3DFORMAT; typedef int D3DPRIMITIVETYPE;
#define D3DERR_INVALIDCALL ((HRESULT)-7)
typedef struct { int marker; } D3DPRESENT_PARAMETERS;
typedef struct IDirect3D8 IDirect3D8; typedef struct IDirect3DDevice8 IDirect3DDevice8;
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
 HRESULT (*DrawPrimitive)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,UINT);
 HRESULT (*DrawIndexedPrimitive)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
 HRESULT (*DrawPrimitiveUP)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
 HRESULT (*DrawIndexedPrimitiveUP)(IDirect3DDevice8*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
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
 HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE,UINT,UINT);
 HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
 HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE,UINT,const void*,UINT);
 HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
#endif
};
