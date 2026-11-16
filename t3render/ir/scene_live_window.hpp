#pragma once

#include "scene_live.hpp"

#include <optional>

namespace t3render::live {

// A thread ID can be recycled: callers must also supply its creation identity.
// These values are process-local synchronization evidence, never SceneIR IDs.
struct ThreadToken {
    std::uint32_t id = 0;
    std::uint64_t birth = 0;
};
struct Viewport {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    float min_depth = 0, max_depth = 1;
};
struct ProbeCamera {
    Matrix view{}, projection{};
};
enum class WindowResult { Primed, Published, Rejected };
enum WindowReject : std::uint32_t {
    WindowNotPrimed = 1u << 0, WindowPresentFailed = 1u << 1, WindowEpochInvalid = 1u << 2,
    WindowTainted = 1u << 3, WindowBoundaryAmbiguous = 1u << 4, WindowPending = 1u << 5,
    WindowNoCamera = 1u << 6, WindowNoViewport = 1u << 7, WindowPublishFailed = 1u << 8,
    WindowNeedsRebind = 1u << 9, WindowBadTicket = 1u << 10
};

// Collects one chosen pass/stage between actual Present boundaries. All
// observations and semantic bindings are supplied by the hook adapter; this
// class does not inspect the game or independently establish their truth.
// A single external producer must call every relevant begin/end or MarkLost.
class SceneWindow {
public:
    SceneWindow(SceneResidency& residency, Epochs epochs, FrameProvenance provenance,
                Conventions conventions, std::uint32_t render_pass, std::uint32_t stage);

    // First successful Present primes the next interval. frame_id is the ID
    // of the frame being presented; consecutive windows must be consecutive.
    // The adapter must attest no untracked callback is still in flight.
    std::uint64_t BeginPresent(ThreadToken thread, std::uint64_t frame_id, bool adapter_quiescent);
    WindowResult EndPresent(std::uint64_t ticket, ThreadToken thread, bool succeeded);

    std::uint64_t BeginProbe(ThreadToken thread);
    bool EndProbe(std::uint64_t ticket, ThreadToken thread, bool pass_known, std::uint32_t render_pass,
                  bool stable, const ProbeCamera& camera);

    // The adapter has already matched one semantic section to its scope.
    // A successful draw and stable scope end are still required to select it.
    std::uint64_t BeginMesh(ThreadToken thread, std::uint32_t render_pass, std::uint32_t stage,
                            const MeshSelection& section);
    std::uint64_t BeginBsp(ThreadToken thread, std::uint32_t render_pass, std::uint32_t stage,
                           const BspSelection& section);
    std::uint64_t BeginDraw(ThreadToken thread);
    // For a selected scope, geometry and camera must match the independent
    // draw-state checks; outside a scope those checks are ignored.
    bool EndDraw(std::uint64_t ticket, ThreadToken thread, bool succeeded,
                 bool scoped_geometry_matches, bool scoped_camera_matches, bool scoped_transform_matches,
                 bool viewport_stable, const Viewport& viewport);
    bool EndScope(std::uint64_t ticket, ThreadToken thread, bool stable);

    // Hook loss, overlap, reentry, device replacement or untracked Reset
    // requires a new window after all old callbacks have drained. Recreate
    // this window after a map or device epoch change.
    void MarkLost();
    bool needs_rebind() const;
    std::uint32_t last_rejection() const;

private:
    void RequireRebind(); // mutex held
    std::uint64_t NextTicket();
    bool SameThread(ThreadToken a, ThreadToken b) const;
    bool Eligible(ThreadToken thread);
    bool MergeScope();
    void ClearInterval();

    SceneResidency& residency_;
    const Epochs epochs_;
    const FrameProvenance provenance_;
    const Conventions conventions_;
    const std::uint32_t pass_, stage_;
    mutable std::mutex mutex_;
    std::uint64_t next_ticket_ = 1;
    bool primed_ = false, tainted_ = false, boundary_ambiguous_ = false, active_probe_matches_ = false;
    bool rebind_required_ = false, draw_scoped_ = false;
    std::uint32_t last_rejection_ = 0;
    std::uint64_t draw_scope_ticket_ = 0;
    ThreadToken previous_thread_{}, present_thread_{}, probe_thread_{}, scope_thread_{}, draw_thread_{};
    std::uint64_t previous_frame_ = 0, present_frame_ = 0;
    std::uint64_t present_ticket_ = 0, probe_ticket_ = 0, scope_ticket_ = 0, draw_ticket_ = 0;
    std::optional<ProbeCamera> camera_;
    std::optional<Viewport> viewport_;
    std::optional<MeshSelection> mesh_;
    std::optional<BspSelection> bsp_;
    std::uint32_t scope_draws_ = 0;
    std::vector<MeshSelection> meshes_;
    std::vector<BspSelection> blocks_;
    std::unordered_map<std::string, std::size_t> mesh_index_, block_index_;
};

} // namespace t3render::live
