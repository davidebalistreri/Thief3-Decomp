#pragma once

// Read-only translation of the already researched probe's transient values
// into the live scene model. The catalog contains offline asset data; none of
// the transient process addresses below may escape into a snapshot or key.
#include "../ir/scene_live_window.hpp"
#include "scene_bridge_submission.hpp"

namespace t3render::live {
struct StaticMeshCandidate {
    std::string instance_id;
    std::shared_ptr<const Resource> resource;
    std::vector<std::optional<std::string>> material_ids;
};
struct BindingCatalog {
    std::string map, map_sha256, bed_instance_id;
    std::shared_ptr<const Resource> bed;
    std::vector<std::optional<std::string>> bed_materials;
    std::unordered_map<std::string, Material> materials;
    // Must be the complete imported BSP catalog for the map. Completeness is
    // asserted by its producer; the legacy importer itself remains heuristic.
    bool bsp_catalog_complete = false;
    std::vector<std::shared_ptr<const Resource>> bsp;
    // Offline candidates only: neither the actor, resource nor materials are
    // considered observed until a same-frame runtime matcher corroborates them.
    std::unordered_map<std::string,StaticMeshCandidate> static_mesh_actors;
};

// Accept only the verified caller, pass and stable camera observation. It does
// not establish a Present interval; SceneWindow handles that separately.
bool ReadProbeCamera(const scene_bridge::ProbeSnapshot& snapshot, std::uint32_t thread_id,
                     std::uint32_t render_pass, ProbeCamera& camera);

// At +0x24 scope entry, match the sampled entry to the current probe and
// offline metadata. The adapter retains the transient entry sample until
// ScopeUnchanged at exit, and brackets successful matching draws and Present
// through SceneWindow. Bindings remain candidates: runtime fingerprints and
// payloads are not observed by these calls.
bool ResolveBed(const BindingCatalog& catalog, const scene_bridge::ProbeSnapshot& snapshot,
                const scene_bridge::SubmissionSample& before, std::uint32_t section,
                std::uint64_t instance_generation, MeshSelection& output);
// Requires a bounded same-scope sample plus the offline actor/resource/skin
// candidate. Success is Candidate evidence, not proof of the GPU payload.
bool ResolveStaticMesh(const BindingCatalog& catalog,const scene_bridge::ProbeSnapshot& snapshot,
                       const scene_bridge::SubmissionSample& before,const scene_bridge::ProbeStaticMesh& observed,
                       std::uint32_t section,std::uint64_t instance_generation,MeshSelection& output);
bool ResolveBsp(const BindingCatalog& catalog, const scene_bridge::ProbeSnapshot& snapshot,
                const scene_bridge::SubmissionSample& before, std::uint32_t section, BspSelection& output);
struct IndexedDraw {
    bool indexed = false, triangle_list = false;
    std::uint32_t first_index = 0, triangle_count = 0, min_vertex = 0, vertex_count = 0;
};
// Compares draw arguments and offline indices only, not the actual GPU input.
bool ScopeDrawMatches(const Resource& resource, std::uint32_t section, const IndexedDraw& draw) noexcept;
// Check both the pre-draw and post-draw sampled camera against the probe.
bool ScopeCameraMatches(const ProbeCamera& camera, const Matrix& view, const Matrix& projection) noexcept;
bool ScopeWorldMatches(const scene_bridge::SubmissionSample& before, const Matrix& world) noexcept;
bool ScopeUnchanged(const scene_bridge::SubmissionSample& before,
                    const scene_bridge::SubmissionSample& after) noexcept;
} // namespace t3render::live
