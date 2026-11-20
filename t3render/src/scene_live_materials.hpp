#pragma once

#include "../ir/scene_live.hpp"
#include "scene_packet.hpp"

namespace t3render::live {

// Viewer-only selection. A retained anchor is a visual heuristic, not
// same-frame evidence about the bed or BSP bindings.
struct BedroomAnchor {
    std::array<double, 3> origin{};
    std::array<double, 9> inverse{};
};
enum class BedroomAnchorSource { None, Current, Retained };
class BedroomAnchorTracker {
public:
    BedroomAnchorSource Select(const std::shared_ptr<const Snapshot>& snapshot, BedroomAnchor& output);
    void Reset() noexcept;
private:
    std::shared_ptr<const Snapshot> witness_;
    BedroomAnchor anchor_{};
};
struct BedroomMaterialRun {
    std::uint32_t first_index = 0, index_count = 0;
    std::optional<std::string> replacement;
    bool head_wall = false;
};
// Offline visual assets kept outside semantic residency. Channel order is
// albedo (sRGB), tangent normal, roughness, ambient occlusion (linear).
struct BedroomMaterial {
    std::string id;
    std::array<std::string,4> source_sha256;
    std::array<scene::Texture,4> channels;
};
// Original diffuse textures used only by the v0.1 bedroom preview.
inline constexpr std::array<const char*,12> kBedroomOriginalNames{{
    "BF", "CIVGoodness_D", "GENbeamA_D", "GENbreadcrust_D", "GENgoodnessA_D", "GENgoodnessBOOK1_D",
    "GENtarget_D", "GENwindowint_D", "HSEfloorB_D", "HSEGoodnessA_D", "STMtrimA_D", "STMwallB_D"
}};
struct BedroomOriginalMaterial {
    std::string id, source_sha256;
    scene::Texture albedo;
};
struct BedroomMaterialLibrary {
    std::array<BedroomMaterial,2> materials;
    // Optional, fixed to the generated bed's wool and linen sections.
    std::optional<std::array<BedroomMaterial,2>> bed_textiles;
    std::optional<std::array<BedroomOriginalMaterial,kBedroomOriginalNames.size()>> original_materials;
    // Generated metal albedo and tinted original wax luminance for the preview candlestick.
    std::optional<BedroomOriginalMaterial> candlestick_metal;
    std::optional<BedroomOriginalMaterial> candlestick_wax;
    std::optional<scene::Texture> wall_height;
    std::string wall_height_source_sha256;
};

bool FindBedroomAnchor(const Snapshot& snapshot, BedroomAnchor& output);
// Partition one BSP section into contiguous index ranges. The material names
// are semantic asset names, not draw numbers or runtime object addresses.
bool PartitionBedroomMaterial(const BedroomAnchor& anchor, const Resource& bsp,
                              std::uint32_t section, const std::string& material_name,
                              std::vector<BedroomMaterialRun>& output, bool identify_head_wall = false);

} // namespace t3render::live
