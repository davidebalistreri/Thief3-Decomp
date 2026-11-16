#pragma once

#include "../ir/scene_live.hpp"
#include "scene_packet.hpp"

namespace t3render::live {

// Viewer-only selection; values are computed from the same snapshot as the
// BSP draw and are never retained as evidence for a later frame.
struct BedroomAnchor {
    std::array<double, 3> origin{};
    std::array<double, 9> inverse{};
};
struct BedroomMaterialRun {
    std::uint32_t first_index = 0, index_count = 0;
    std::optional<std::string> replacement;
};
// Offline visual assets kept outside semantic residency. Channel order is
// albedo (sRGB), tangent normal, roughness, ambient occlusion (linear).
struct BedroomMaterial {
    std::string id;
    std::array<std::string,4> source_sha256;
    std::array<scene::Texture,4> channels;
};
struct BedroomMaterialLibrary {
    std::array<BedroomMaterial,2> materials;
};

bool FindBedroomAnchor(const Snapshot& snapshot, BedroomAnchor& output);
// Partition one BSP section into contiguous index ranges. The material names
// are semantic asset names, not draw numbers or runtime object addresses.
bool PartitionBedroomMaterial(const BedroomAnchor& anchor, const Resource& bsp,
                              std::uint32_t section, const std::string& material_name,
                              std::vector<BedroomMaterialRun>& output);

} // namespace t3render::live
