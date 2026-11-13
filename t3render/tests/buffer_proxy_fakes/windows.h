#pragma once

#include <cstddef>
#include <cstdint>

#define STDMETHODCALLTYPE
#define WINAPI
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)

using BYTE = std::uint8_t;
using UINT = std::uint32_t;
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using HRESULT = std::int32_t;

inline thread_local DWORD test_last_error = 0;
inline DWORD GetLastError() { return test_last_error; }
inline void SetLastError(DWORD value) { test_last_error = value; }

struct GUID { int value; };
using IID = GUID;
using REFIID = const IID&;
using REFGUID = const GUID&;
inline bool IsEqualIID(REFIID left, REFIID right) { return left.value == right.value; }

struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
inline constexpr HRESULT S_OK = 0;
inline constexpr HRESULT E_FAIL = -1;
inline constexpr HRESULT E_NOINTERFACE = -2;
inline constexpr HRESULT E_POINTER = -3;
inline constexpr HRESULT E_OUTOFMEMORY = -4;
inline constexpr HRESULT E_NOTIMPL = -6;
inline constexpr IID IID_IUnknown{1};
