#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#define WINAPI
#define STDMETHODCALLTYPE
#define SUCCEEDED(h) ((h)>=0)
#define FAILED(h) ((h)<0)
#define MEM_COMMIT 0x1000
#define PAGE_GUARD 0x100
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define MAX_PATH 260
typedef uint32_t UINT; typedef uint32_t DWORD; typedef int32_t HRESULT; typedef uint32_t ULONG; typedef void* HWND; typedef void* HMODULE; typedef const wchar_t* LPCWSTR;
typedef struct { int x; } RECT; typedef struct { int x; } RGNDATA;
typedef struct { void* BaseAddress; size_t RegionSize; DWORD State,Protect; } MEMORY_BASIC_INFORMATION;
typedef struct { int id; } IID;
typedef void* FARPROC;
#ifdef __cplusplus
#define REFIID const IID&
extern "C" {
#else
#define REFIID const IID*
#endif
extern const IID IID_IDirect3D8, IID_IDirect3DDevice8;
size_t VirtualQuery(void*,MEMORY_BASIC_INFORMATION*,size_t);
void* InterlockedCompareExchangePointer(void* volatile*,void*,void*);
void OutputDebugStringA(const char*);
DWORD GetLastError(void);
void SetLastError(DWORD);
UINT GetSystemDirectoryW(wchar_t*, UINT);
HMODULE LoadLibraryW(const wchar_t*);
int FreeLibrary(HMODULE);
int GetModuleHandleExW(DWORD,LPCWSTR,HMODULE*);
void* GetProcAddress(HMODULE,const char*);
#ifdef __cplusplus
}
inline bool IsEqualIID(REFIID a, REFIID b) { return a.id==b.id; }
#endif

#define E_FAIL ((HRESULT)-1)
