#pragma once

#include "scene_live_catalog.hpp"

#include <memory>

namespace t3render::live {

// Copied draw-state observations. No native interfaces or engine addresses
// are retained by a published scene snapshot.
struct DrawObservation {
    bool available = false;
    Matrix view{}, projection{}, world{};
    Viewport viewport;
};
struct DrawStart {
    std::uint64_t ticket = 0;
    bool needs_state = false;
};
struct DriverStats {
    std::uint64_t probes = 0, selected_probes = 0, selected_probe_bad = 0, bed_scopes = 0, bsp_scopes = 0, bsp_cache_hits = 0;
    std::uint64_t draw_geometry_bad = 0, draw_camera_bad = 0, draw_world_bad = 0, draw_viewport_bad = 0;
    std::uint64_t scope_unstable = 0, scope_rejected = 0, losses = 0, primed = 0, published = 0, rejected = 0;
    std::uint32_t last_rejection = 0;
};

// Connects the established probe/submission samples and device boundaries to
// a semantic residency. Every begin must be paired with its end, including
// ignored scopes and unselected draws. This driver is not itself a hook or a
// D3D12 renderer; the adapter owns native calls and supplies copied samples.
class SceneDriver {
public:
    static std::unique_ptr<SceneDriver> Create(LoadedCatalog catalog, FrameProvenance provenance,
                                               std::uint32_t render_pass, std::uint32_t stage) noexcept;
    ~SceneDriver();
    SceneDriver(const SceneDriver&) = delete;
    SceneDriver& operator=(const SceneDriver&) = delete;

    std::uint64_t BeginProbe(ThreadToken thread) noexcept;
    void EndProbe(std::uint64_t ticket, ThreadToken thread, const scene_bridge::ProbeSnapshot& snapshot) noexcept;
    // Begin the scope before sampling process memory, so Present cannot pass
    // an in-flight sample unnoticed. SetScopeSample must follow exactly once.
    std::uint64_t BeginScope(ThreadToken thread, std::uint32_t stage, std::uint32_t section) noexcept;
    bool SetScopeSample(std::uint64_t ticket, ThreadToken thread,
                        const scene_bridge::SubmissionSample& before) noexcept;
    void EndScope(std::uint64_t ticket, ThreadToken thread, const scene_bridge::SubmissionSample& after) noexcept;
    DrawStart BeginDraw(ThreadToken thread, std::uint64_t enclosing_scope, const IndexedDraw& draw) noexcept;
    void EndDraw(std::uint64_t ticket, ThreadToken thread, bool succeeded,
                 const DrawObservation& before, const DrawObservation& after) noexcept;
    std::uint64_t BeginPresent(ThreadToken thread) noexcept;
    WindowResult EndPresent(std::uint64_t ticket, ThreadToken thread, bool succeeded) noexcept;
    std::uint64_t BeginReset(ThreadToken thread) noexcept;
    void EndReset(std::uint64_t ticket, ThreadToken thread, bool succeeded) noexcept;
    // Call for hook loss, reentry, untracked operations or a replaced device.
    void MarkLost() noexcept;
    std::shared_ptr<const Snapshot> current() const noexcept;
    DriverStats stats() const noexcept;
    bool SubmitIfCurrent(const std::shared_ptr<const Snapshot>& expected,
                         bool (*submit)(void*) noexcept, void* context) noexcept;

private:
    struct Impl;
    explicit SceneDriver(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace t3render::live
