#pragma once

#include "scene_packet.hpp"
#include <memory>
#include <vector>
#include <windows.h>

namespace t3render::live {
enum class FlatPresent { Presented, Busy, Failed };

// Persistent diagnostic backend. A null window renders offscreen; otherwise
// the same D3D12 target is copied into a DXGI swap chain. Calls must be made
// from one thread. No game objects or native D3D8 resources are retained.
class FlatD3D12 {
public:
    static std::unique_ptr<FlatD3D12> Create(HWND window) noexcept;
    ~FlatD3D12();
    FlatD3D12(const FlatD3D12&) = delete;
    FlatD3D12& operator=(const FlatD3D12&) = delete;

    // Prepare records commands without submitting them. Submit is kept
    // separate so a caller can synchronize it with scene epoch invalidation.
    // Finish waits outside that synchronization; pixels, when requested, are
    // tightly packed RGBA8 for fixture verification. Calls must be paired.
    bool Prepare(const scene::Frame& frame, bool readback = false) noexcept;
    bool Submit() noexcept;
    bool Finish(std::vector<std::uint8_t>* pixels = nullptr) noexcept;
    // Present is deliberately outside the scene lock: a driver or windowing
    // system can stall even when DO_NOT_WAIT is requested.
    FlatPresent Present() noexcept;
    void Cancel() noexcept;
    // Convenience for isolated fixtures. Empty selections clear the image.
    // Failed submissions disable this object.
    bool Render(const scene::Frame& frame, std::vector<std::uint8_t>* pixels = nullptr) noexcept;

private:
    struct Impl;
    explicit FlatD3D12(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace t3render::live
