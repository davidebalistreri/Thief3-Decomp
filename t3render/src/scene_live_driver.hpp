#pragma once

#include "scene_live_catalog.hpp"
#include "scene_live_light_raw.hpp"

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
    bool needs_state = false, needs_world = false;
};
struct DriverStats {
    std::uint64_t probes = 0, selected_probes = 0, selected_probe_bad = 0, bed_scopes = 0, bsp_scopes = 0, bsp_cache_hits = 0;
    std::uint64_t static_mesh_samples = 0, static_mesh_scopes = 0, static_mesh_rejected = 0;
    std::uint64_t map_witness_conflicts = 0;
    std::uint64_t unscoped_viewport_attempts = 0;
    std::uint64_t map_transitions = 0;
    std::uint64_t draw_geometry_bad = 0, draw_camera_bad = 0, draw_world_bad = 0, draw_viewport_bad = 0;
    std::uint64_t scope_unstable = 0, scope_rejected = 0, losses = 0, primed = 0, published = 0, rejected = 0;
    std::uint32_t last_rejection = 0;
};
struct VisualAssets {
    std::shared_ptr<const MeshOverride> bed_override;
    std::shared_ptr<const BedroomMaterialLibrary> bedroom_materials;
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
    // Future adapters must bracket the full sample after the selected probe.
    // Coverage and stability remain producer attestations. The optional raw
    // input must not be mutated during this call; the driver keeps its own copy.
    // When requested, generation is fresh per Present interval; it does not
    // claim persistence of the underlying actor across frames.
    std::uint64_t BeginLightSample(ThreadToken thread, std::uint64_t* generation = nullptr) noexcept;
    bool EndLightSample(std::uint64_t ticket, ThreadToken thread, bool stable, LightCoverage coverage,
                        const std::vector<VisualLight>& lights,
                        std::shared_ptr<const RawLightBatch> raw = {}) noexcept;
    bool CancelLightSample(std::uint64_t ticket, ThreadToken thread) noexcept;
    // Begin the scope before sampling process memory, so Present cannot pass
    // an in-flight sample unnoticed. SetScopeSample must follow exactly once.
    std::uint64_t BeginScope(ThreadToken thread, std::uint32_t stage, std::uint32_t section) noexcept;
    // Returns the current immutable probe only for a catalogued non-bed actor
    // inside this scope. An adapter may then take the expensive bounded sample.
    std::shared_ptr<const scene_bridge::ProbeSnapshot> GenericProbe(std::uint64_t ticket,ThreadToken thread,
                        const scene_bridge::SubmissionSample& before) noexcept;
    bool SetScopeSample(std::uint64_t ticket, ThreadToken thread,
                        const scene_bridge::SubmissionSample& before,
                        const scene_bridge::ProbeStaticMesh* generic = nullptr) noexcept;
    void EndScope(std::uint64_t ticket, ThreadToken thread, const scene_bridge::SubmissionSample& after) noexcept;
    DrawStart BeginDraw(ThreadToken thread, std::uint64_t enclosing_scope, const IndexedDraw& draw) noexcept;
    void EndDraw(std::uint64_t ticket, ThreadToken thread, bool succeeded,
                 const DrawObservation& before, const DrawObservation& after) noexcept;
    // When supplied, frame_id receives the window's ID for this very Present,
    // or UINT64_MAX if it could not be attached to a window.
    std::uint64_t BeginPresent(ThreadToken thread, std::uint64_t* frame_id = nullptr) noexcept;
    WindowResult EndPresent(std::uint64_t ticket, ThreadToken thread, bool succeeded) noexcept;
    std::uint64_t BeginReset(ThreadToken thread) noexcept;
    void EndReset(std::uint64_t ticket, ThreadToken thread, bool succeeded) noexcept;
    // The adapter must supply a real map lifecycle signal; a candidate map
    // witness alone is not one. Begin immediately retires the current frame.
    // Commit consumes a validated, unused catalog only after callbacks drain.
    bool BeginMapTransition() noexcept;
    bool CommitMapTransition(LoadedCatalog& prepared) noexcept;
    // Call for hook loss, reentry, untracked operations or a replaced device.
    void MarkLost() noexcept;
    std::shared_ptr<const Snapshot> current() const noexcept;
    bool VisualAssetsIfCurrent(const std::shared_ptr<const Snapshot>& expected, VisualAssets& output) const noexcept;
    // Only the raw diagnostics attached to this exact current snapshot are
    // returned. No diagnostic data enters the SceneIR or the lighting path.
    std::shared_ptr<const RawLightFrame> LightDiagnosticsIfCurrent(const std::shared_ptr<const Snapshot>& expected) const noexcept;
    DriverStats stats() const noexcept;
    bool SubmitIfCurrent(const std::shared_ptr<const Snapshot>& expected,
                         bool (*submit)(void*) noexcept, void* context) noexcept;

private:
    struct Impl;
    explicit SceneDriver(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace t3render::live
