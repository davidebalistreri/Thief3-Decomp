#include <stddef.h>
#include <windows.h>
#include <d3d8.h>
#include "vtable_layout.h"

#define SLOT(type, member) (offsetof(struct type##Vtbl, member) / sizeof(void *))

T3VtableLayout T3GetVtableLayout(void) {
    const T3VtableLayout result = {
        sizeof(struct IDirect3D8Vtbl) / sizeof(void *),
        SLOT(IDirect3D8, QueryInterface), SLOT(IDirect3D8, Release), SLOT(IDirect3D8, CreateDevice),
        sizeof(struct IDirect3DDevice8Vtbl) / sizeof(void *),
        SLOT(IDirect3DDevice8, QueryInterface), SLOT(IDirect3DDevice8, Release),
        SLOT(IDirect3DDevice8, Reset), SLOT(IDirect3DDevice8, Present),
        SLOT(IDirect3DDevice8, DrawPrimitive), SLOT(IDirect3DDevice8, DrawIndexedPrimitive),
        SLOT(IDirect3DDevice8, DrawPrimitiveUP), SLOT(IDirect3DDevice8, DrawIndexedPrimitiveUP)
    };
    return result;
}
