#pragma once

#include <cstdint>
#include <cstring>

#define STDMETHODCALLTYPE

using UINT = std::uint32_t;
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using HRESULT = std::int32_t;
using BOOL = std::int32_t;
using HWND = void*;
using HMONITOR = void*;

inline thread_local DWORD fake_last_error = 0;
inline DWORD GetLastError() { return fake_last_error; }
inline void SetLastError(DWORD value) { fake_last_error = value; }

struct GUID { std::uint32_t a; std::uint16_t b, c; std::uint8_t d[8]; };
using REFIID = const GUID&;
inline bool IsEqualIID(REFIID a, REFIID b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
inline constexpr GUID IID_IUnknown{1, 0, 0, {}};
inline constexpr HRESULT S_OK = 0;
inline constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003u);
inline constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002u);
inline constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001u);
inline constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);

struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
protected:
    ~IUnknown() = default;
};
