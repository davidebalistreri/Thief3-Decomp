#include "scene_live_catalog_win.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <vector>

namespace t3render::live {
namespace {
bool Sha256(const std::uint8_t* bytes, std::size_t size, std::uint8_t* output, void*) noexcept {
    if (!bytes || !size || size>kMaxCatalogBytes || !output) return false;
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;bool ok=false;
    if (CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT|CRYPT_SILENT) &&
        CryptCreateHash(provider,CALG_SHA_256,0,0,&hash) &&
        CryptHashData(hash,bytes,static_cast<DWORD>(size),0)) {
        DWORD length=32;
        ok=CryptGetHashParam(hash,HP_HASHVAL,output,&length,0) && length==32;
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider,0);
    return ok;
}
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~File() { if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct LastError {
    DWORD value=GetLastError();
    ~LastError() { SetLastError(value); }
};
} // namespace

bool LoadCatalogFile(const wchar_t* path, LoadedCatalog& output) noexcept {
    LastError saved;
    try {
        if (!path || !*path) return false;
        File file{CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
        if (file.handle==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.handle,&length) || length.QuadPart<40 || length.QuadPart>kMaxCatalogBytes) return false;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length.QuadPart));
        std::size_t offset=0;
        while (offset<bytes.size()) {
            DWORD count=0;
            const auto remaining=static_cast<DWORD>(bytes.size()-offset);
            if (!ReadFile(file.handle,bytes.data()+offset,remaining,&count,nullptr) || !count) return false;
            offset+=count;
        }
        std::uint8_t extra=0;DWORD count=0;
        if (!ReadFile(file.handle,&extra,1,&count,nullptr) || count) return false;
        return LoadCatalog(bytes.data(),bytes.size(),Sha256,nullptr,output);
    } catch (...) { return false; }
}
} // namespace t3render::live
