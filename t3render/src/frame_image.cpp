#include "frame_image.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace t3render {
namespace {
template<class T> struct ComRef {
    T* value = nullptr;
    ~ComRef() { if (value) value->Release(); }
};

bool WriteAll(HANDLE file, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 0x7fffffff));
        DWORD written = 0;
        if (!WriteFile(file, bytes, chunk, &written, nullptr) || !written) return false;
        bytes += written;
        size -= written;
    }
    return true;
}
}

FrameImageResult ReadFrameImage(IDirect3DDevice8* device, const std::wstring& path, std::uint64_t frame) {
    FrameImageResult result;
    try {
        ComRef<IDirect3DSurface8> back, image;
        result.stage = "get_backbuffer";
        result.hr = device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back.value);
        if (FAILED(result.hr) || !back.value) return result;
        D3DSURFACE_DESC desc{};
        result.stage = "get_description";
        result.hr = back.value->GetDesc(&desc);
        if (FAILED(result.hr)) return result;
        result.width = desc.Width; result.height = desc.Height; result.format = desc.Format;
        result.stage = "unsupported_format_or_size";
        if (!desc.Width || !desc.Height || desc.Width > 8192 || desc.Height > 8192 ||
            std::uint64_t(desc.Width) * desc.Height > 16777216 || desc.MultiSampleType != D3DMULTISAMPLE_NONE ||
            (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) { result.hr = E_FAIL; return result; }

        result.stage = "create_image_surface";
        result.hr = device->CreateImageSurface(desc.Width, desc.Height, desc.Format, &image.value);
        if (FAILED(result.hr) || !image.value) return result;
        result.stage = "copy_backbuffer";
        result.hr = device->CopyRects(back.value, nullptr, 0, image.value, nullptr);
        if (FAILED(result.hr)) return result;
        std::vector<std::uint8_t> rgb(std::size_t(desc.Width) * desc.Height * 3);
        D3DLOCKED_RECT locked{};
        result.stage = "lock_image";
        result.hr = image.value->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(result.hr)) return result;
        const bool valid = locked.pBits && locked.Pitch >= static_cast<INT>(desc.Width * 4) &&
            (desc.Height == 1 || static_cast<std::size_t>(locked.Pitch) <=
             static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / (desc.Height - 1));
        if (valid) {
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(locked.pBits) + std::ptrdiff_t(y) * locked.Pitch;
                for (UINT x = 0; x < desc.Width; ++x) {
                    const auto i = (std::size_t(y) * desc.Width + x) * 3;
                    rgb[i] = row[x * 4 + 2]; rgb[i + 1] = row[x * 4 + 1]; rgb[i + 2] = row[x * 4];
                }
            }
        }
        result.stage = "unlock_image";
        result.hr = image.value->UnlockRect();
        if (FAILED(result.hr)) return result;
        if (!valid) { result.stage = "invalid_pitch"; result.hr = E_FAIL; return result; }

        const std::wstring target = path + L".frame-" + std::to_wstring(frame) + L".ppm";
        const std::string header = "P6\n" + std::to_string(desc.Width) + " " + std::to_string(desc.Height) + "\n255\n";
        result.stage = "create_file";
        HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { result.hr = HRESULT_FROM_WIN32(GetLastError()); return result; }
        result.stage = "write_file";
        const bool written = WriteAll(file, header.data(), header.size()) && WriteAll(file, rgb.data(), rgb.size());
        const DWORD write_error = written ? ERROR_SUCCESS : GetLastError();
        const bool closed = CloseHandle(file) != FALSE;
        if (!written || !closed) { result.hr = HRESULT_FROM_WIN32(written ? GetLastError() : write_error); return result; }
        result.status = "captured";
        result.stage = "complete";
        result.hr = S_OK;
        return result;
    } catch (...) { result.stage = "exception"; return result; }
}
}
