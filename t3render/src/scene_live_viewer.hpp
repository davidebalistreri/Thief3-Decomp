#pragma once

#include "scene_live_driver.hpp"
#include <memory>
#include <string>
#include <windows.h>

namespace t3render::live {
using SubmitOperation = bool (*)(void*) noexcept;
struct ViewerProvider {
    // The provider remains alive until the worker exits, even if Viewer is destroyed.
    std::shared_ptr<void> context;
    std::shared_ptr<const Snapshot> (*current)(void*) noexcept = nullptr;
    bool (*submit_if_current)(void*, const std::shared_ptr<const Snapshot>&,
                              SubmitOperation, void*) noexcept = nullptr;
    DriverStats (*driver_stats)(void*) noexcept = nullptr;
    // Optional diagnostic side channel; it must match the exact snapshot.
    std::shared_ptr<const RawLightFrame> (*light_diagnostics)(void*,const std::shared_ptr<const Snapshot>&) noexcept = nullptr;
    // Optional exact-snapshot catalog assets, for map transitions/reloads.
    bool (*visual_assets)(void*,const std::shared_ptr<const Snapshot>&,VisualAssets&) noexcept = nullptr;
    std::shared_ptr<const MeshOverride> bed_override;
    std::shared_ptr<const BedroomMaterialLibrary> bedroom_materials;
    bool bedroom_material_ids = false;
    // Explicit viewer-only PBR preview; never used as game light evidence.
    bool preview_pbr = false;
    bool preview_candle = false; // explicit candidate-only, artistic preview
    bool wall_displacement = false; // opt-in viewer-only head-wall geometry
    bool preview_gi = false; // experimental screen-space bedroom bounce
    // Optional existing local directory; copied matrices, candidate light
    // words and flat images contain no engine-object addresses.
    std::wstring diagnostic_directory;
    // When the D3D8 capture marker appears, take a bounded series of viewer
    // images so a shared Present can be checked after the run.
    std::wstring diagnostic_capture_trigger;
    // Optional exact-Present selection for a distributed diagnostic capture.
    bool (*diagnostic_capture_frame)(void*, std::uint64_t) noexcept = nullptr;
    // Optional file-controlled flat/remaster switch, applied only by the viewer.
    std::wstring remaster_toggle_path;
    // Capture mode retains initial images; timed live-only runs can disable
    // these readbacks without disabling a later explicit capture trigger.
    bool diagnostic_initial_images = true;
    // GPU timing and DXGI memory queries are separate opt-in diagnostics.
    bool diagnostic_gpu_metrics = false;
    // Process-session identity shared with the trace, never an engine pointer.
    // Required when diagnostics are requested.
    std::string diagnostic_session_id;
};
struct ViewerStats {
    bool initialized = false, failed = false, stopped = false;
    std::uint64_t frames = 0, dropped = 0, cleared = 0, resident_retirements = 0, renderer_rebuilds = 0;
    std::uint64_t map_epoch = 0, device_epoch = 0, frame_id = 0;
    std::uint32_t bedroom_material_runs = 0, textured_draws = 0, pbr_draws = 0, shadowed_lights = 0;
    std::uint32_t wall_displaced_triangles = 0;
    bool wall_displacement_failed = false;
    bool preview_gi_active = false;
};

// Dedicated worker for the experimental flat/albedo or verified-light preview. It consumes the latest
// available snapshot; snapshots superseded during preparation are dropped.
// D3D12 and window calls do not run on the game's render thread.
class SceneViewer {
public:
    static std::unique_ptr<SceneViewer> Start(ViewerProvider provider, bool window) noexcept;
    ~SceneViewer();
    SceneViewer(const SceneViewer&) = delete;
    SceneViewer& operator=(const SceneViewer&) = delete;
    void Notify() noexcept;
    void Stop() noexcept;
    ViewerStats stats() const noexcept;
private:
    struct State;
    static DWORD WINAPI Run(void*) noexcept;
    explicit SceneViewer(std::shared_ptr<State>, void* thread) noexcept;
    std::shared_ptr<State> state_;
    void* thread_ = nullptr;
};
} // namespace t3render::live
