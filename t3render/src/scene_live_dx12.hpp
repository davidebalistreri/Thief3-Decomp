#pragma once

#include "scene_live_flat.hpp"
#include "scene_live_materials.hpp"
#include <memory>
#include <vector>
#include <windows.h>

namespace t3render::live {
struct RawLightFrame;
enum class FlatPresent { Presented, Busy, Failed };
struct FlatCacheStats {
    std::uint64_t uploads = 0, evictions = 0;
    std::size_t resources = 0, bytes = 0;
    std::uint32_t textured_draws = 0, pbr_draws = 0;
    std::uint32_t shadowed_lights = 0, shadow_draws = 0;
    bool shadow_budget_exceeded = false;
    // GPU command-list duration; negative means unavailable. This includes
    // any image readback requested for the frame, but not DXGI Present.
    double gpu_frame_ms = -1.0;
    bool gpu_frame_includes_readback = false;
    std::size_t material_bytes = 0;
    // DXGI's local-segment estimate; UINT64_MAX means unavailable.
    std::uint64_t video_memory_usage_bytes = UINT64_MAX;
    // 0: available, 1: disabled, 2: frequency, 3: query heap,
    // 4: readback allocation, 5: map, 6: non-monotonic timestamps,
    // 7: collecting consecutive valid samples.
    std::uint32_t gpu_timing_status = 1;
    std::uint64_t gpu_tick_begin = 0, gpu_tick_end = 0, gpu_tick_frequency = 0;
    std::uint64_t video_memory_sampled_ms = 0;
    bool gi_enabled = false;
};

// Persistent diagnostic backend. A null window renders offscreen; otherwise
// the same D3D12 target is copied into a DXGI swap chain. Calls must be made
// from one thread. No game objects or native D3D8 resources are retained.
class FlatD3D12 {
public:
    static std::unique_ptr<FlatD3D12> Create(HWND window, std::size_t cache_budget = 128u*1024u*1024u,
                                             std::shared_ptr<const BedroomMaterialLibrary> materials = {},
                                             bool gpu_metrics = false, bool preview_gi = false) noexcept;
    ~FlatD3D12();
    FlatD3D12(const FlatD3D12&) = delete;
    FlatD3D12& operator=(const FlatD3D12&) = delete;

    // Prepare records commands without submitting them. Submit is kept
    // separate so a caller can synchronize it with scene epoch invalidation.
    // Finish waits outside that synchronization; pixels, when requested, are
    // tightly packed RGBA8 for fixture verification. Calls must be paired.
    bool Prepare(const scene::Frame& frame, bool readback = false) noexcept;
    bool PrepareResident(const ResidentFlatFrame& frame, bool readback = false) noexcept;
    // Requires complete, verified lighting from this exact snapshot. A false
    // result before command recording leaves the flat path available.
    // synthetic_normal_map enables an explicitly assumed +Y tangent normal
    // convention for synthetic fixtures only; TDS's convention is unknown.
    // Shadows use only selected opaque geometry, a bounded number of lights
    // and draw calls; cache_stats() reports their actual per-frame use.
    bool PrepareLit(const Snapshot& snapshot, const ResidentFlatFrame& frame, bool readback = false,
                    bool synthetic_normal_map = false, bool shadows = true) noexcept;
    // Viewer-only white headlight for an explicit PBR preview. It is not a
    // sampled game light and never changes the snapshot's light evidence.
    bool PreparePreviewLit(const Snapshot& snapshot, const ResidentFlatFrame& frame, bool readback = false) noexcept;
    // Explicit artistic candle preview. A null sample renders no candle
    // contribution; a null sample never asserts a verified disabled state.
    // other_raw is restricted to the other researched bedroom candle and,
    // when raw is also supplied, must refer to the same raw batch.
    bool PrepareCandlePreview(const Snapshot& snapshot, const RawLightFrame* raw,
                              const ResidentFlatFrame& frame, bool readback = false,
                              const RawLightFrame* other_raw = nullptr) noexcept;
    bool Submit() noexcept;
    bool Finish(std::vector<std::uint8_t>* pixels = nullptr) noexcept;
    // Present is deliberately outside the scene lock: a driver or windowing
    // system can stall even when DO_NOT_WAIT is requested.
    FlatPresent Present() noexcept;
    void Cancel() noexcept;
    // Call only with no prepared or in-flight frame; releases cached geometry.
    bool RetireResidents() noexcept;
    // Convenience for isolated fixtures. Empty selections clear the image.
    // Failed submissions disable this object.
    bool Render(const scene::Frame& frame, std::vector<std::uint8_t>* pixels = nullptr) noexcept;
    FlatCacheStats cache_stats() const noexcept;

private:
    struct Impl;
    explicit FlatD3D12(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace t3render::live
