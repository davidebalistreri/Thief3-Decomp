#pragma once

#include "../ir/scene_live.hpp"
#include "scene_live_light_raw.hpp"
#include "scene_live_flat.hpp"

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
// Inverts the view transform without attributing any lighting to the game.
bool CameraPositionFromView(const Matrix& view, Vec3& output) noexcept;
// Artistic preview of one of the two researched bedroom candles. Zero selects
// the configured candle; 6 or 14 select it explicitly. Evidence stays candidate.
bool CandlePreviewFromRaw(const Snapshot& snapshot, const RawLightFrame& raw, RenderLight& output,
                          std::uint32_t actor_number=0) noexcept;
// v0.1 uses the same-frame candidate bLightOn lookup when available; an
// unavailable property can fall back to the game's candidate modulation.
enum class RemasterCandleState { Unknown, Off, On };
RemasterCandleState RemasterCandleFromRaw(const Snapshot& snapshot, const RawLightFrame& raw,
                                         RenderLight& output, std::uint32_t actor_number=0) noexcept;
// A same-frame visual-actor sample can show a zero modulation after the
// light leaves the partial list. This does not prove the enabled property.
bool CandlePreviewDirectZero(const Snapshot& snapshot, const RawLightFrame& raw, std::uint32_t actor_number=0) noexcept;
// Generated metal and wax use the independently sampled visual-actor pose.
// They are viewer-only replacements and do not change the source scene.
bool CandlePreviewCandlestick(const Snapshot& snapshot, const RawLightFrame& raw,
                              std::array<ResidentFlatDraw,2>& output, std::uint32_t actor_number=0) noexcept;
// Uses the generated two-flame mesh only with matching same-frame light and
// visual-actor samples; the geometry and radiance are artistic preview data.
bool CandlePreviewFlame(const Snapshot& snapshot, const RawLightFrame& raw, ResidentFlatDraw& output,
                        std::uint32_t actor_number=0) noexcept;
// Keep the holder in both states; add a flame only when the caller's
// same-frame light state is on and the geometry fits the frame budget.
void AppendCandlePreview(const Snapshot& snapshot, const RawLightFrame& raw,
                         bool candle_on, ResidentFlatFrame& frame, std::uint32_t actor_number=0);
// A producer's Verified flags and Complete coverage are attestations, not
// independent runtime measurements. Failure leaves output untouched.
LightingResult BuildVerifiedLighting(const Snapshot& snapshot, VerifiedLighting& output);
// A light set may be used only for the exact snapshot from which it was built.
bool LightingMatchesSnapshot(const Snapshot& snapshot,const VerifiedLighting& lighting) noexcept;
} // namespace t3render::live
