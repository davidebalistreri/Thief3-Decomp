#pragma once

#include "scene_live_light_raw.hpp"

namespace t3render::live {
// Conservative viewport-normalized bounds of the replacement geometry's
// transformed local AABB. They do not establish occlusion or visible pixels.
struct BedroomFramingBounds { double left=0, top=0, right=0, bottom=0; };
bool BedroomBedFraming(const Snapshot& snapshot, const MeshOverride& override,
                       BedroomFramingBounds& output) noexcept;
bool BedroomCandleFraming(const Snapshot& snapshot, const RawLightFrame& raw,
                          BedroomFramingBounds& output, std::uint32_t actor_number = 0) noexcept;
} // namespace t3render::live
