#pragma once

// Minimal host test doubles, not the Windows ABI or a replacement SDK.
#include <cstdint>
#include <cstddef>

using BYTE = std::uint8_t;
using DWORD = std::uint32_t;
using UINT = std::uint32_t;
using ULONG = std::uint32_t;
using BOOL = std::int32_t;
using HRESULT = std::int32_t;
using HANDLE = int;
using SIZE_T = std::size_t;

struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
};

constexpr BOOL FALSE = 0;
constexpr BOOL TRUE = 1;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
constexpr HANDLE INVALID_HANDLE_VALUE = -1;
constexpr DWORD GENERIC_WRITE = 1;
constexpr DWORD FILE_SHARE_READ = 2;
constexpr DWORD CREATE_NEW = 3;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 4;
constexpr unsigned VK_F11 = 0x7a;
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD PAGE_NOACCESS = 0x01;
constexpr DWORD PAGE_READONLY = 0x02;
constexpr DWORD PAGE_READWRITE = 0x04;
constexpr DWORD PAGE_WRITECOPY = 0x08;
constexpr DWORD PAGE_EXECUTE_READ = 0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD PAGE_EXECUTE_WRITECOPY = 0x80;
constexpr DWORD PAGE_GUARD = 0x100;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_ENVVAR_NOT_FOUND = 203;

#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)

DWORD GetEnvironmentVariableW(const wchar_t*, wchar_t*, DWORD);
DWORD GetLastError();
void SetLastError(DWORD);
HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, void*);
BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
BOOL CloseHandle(HANDLE);
BOOL GetKeyboardState(BYTE*);
SIZE_T VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, SIZE_T);
HANDLE GetCurrentProcess();
BOOL ReadProcessMemory(HANDLE, const void*, void*, SIZE_T, SIZE_T*);
