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
    // Optional existing local directory; copied matrices and at most two flat
    // images are diagnostic output, not a serialized engine-object trace.
    std::wstring diagnostic_directory;
};
struct ViewerStats {
    bool initialized = false, failed = false, stopped = false;
    std::uint64_t frames = 0, dropped = 0, cleared = 0, resident_retirements = 0;
    std::uint64_t map_epoch = 0, device_epoch = 0, frame_id = 0;
};

// Dedicated worker for the experimental flat preview. It consumes the latest
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
