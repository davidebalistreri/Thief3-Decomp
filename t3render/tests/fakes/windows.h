#pragma once

// Minimal host test doubles, not the Windows ABI or a replacement SDK.
#include <cstdint>

using BYTE = std::uint8_t;
using DWORD = std::uint32_t;
using UINT = std::uint32_t;
using ULONG = std::uint32_t;
using BOOL = std::int32_t;
using HRESULT = std::int32_t;
using HANDLE = int;

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

#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)

DWORD GetEnvironmentVariableW(const wchar_t*, wchar_t*, DWORD);
HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, void*);
BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
BOOL CloseHandle(HANDLE);
BOOL GetKeyboardState(BYTE*);
