#pragma once

#include "../ir/scene_live.hpp"

namespace t3render::live {
constexpr std::size_t kMaxRenderLights=16;
struct RenderLight {
    std::string id;
    std::uint64_t generation=0;
    LightKind kind=LightKind::Point;
    Vec3 position{},color_linear{},direction{};
    float intensity=0,influence_radius_m=0,source_radius_m=0,cos_inner=0,cos_outer=0;
};
struct VerifiedLighting {
    std::uint64_t map_epoch=0,device_epoch=0,continuity=0,frame_id=0;
    std::uint32_t render_pass=0,stage=0;
    std::string map,map_sha256;
    FrameProvenance provenance;
    Conventions conventions;
    Camera camera;
    Vec3 camera_position{}; // same coordinates as light positions
    float meters_per_unit=0;
    std::vector<RenderLight> lights;
    std::vector<VisualLight> observations; // exact same-frame evidence, including disabled lights
};
enum class LightingResult { Ready, UnknownCoverage, Incomplete, TooManyLights, InvalidCamera, InvalidUnits, InvalidProvenance };
// A producer's Verified flags and Complete coverage are attestations, not
// independent runtime measurements. Failure leaves output untouched.
LightingResult BuildVerifiedLighting(const Snapshot& snapshot, VerifiedLighting& output);
// A light set may be used only for the exact snapshot from which it was built.
bool LightingMatchesSnapshot(const Snapshot& snapshot,const VerifiedLighting& lighting) noexcept;
} // namespace t3render::live
