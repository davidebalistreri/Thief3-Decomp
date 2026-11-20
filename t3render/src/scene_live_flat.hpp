#pragma once

#include "../ir/scene_live.hpp"
#include "scene_packet.hpp"

namespace t3render::live {
struct BedroomAnchor;
enum class BedroomAnchorSource;
struct ResidentFlatDraw {
    std::shared_ptr<const Resource> resource;
    std::uint32_t section = 0, first_index = 0, index_count = 0;
    Matrix transform{};
    std::optional<std::string> material_id;
    std::array<float,4> color{};
    float emission = 0; // viewer-only HDR radiance; zero for captured scene draws
    std::uint32_t preview_candle_actor = 0; // scoped bedroom preview only
    std::uint8_t wall_relief = 0; // 1: normal only; 2: normal plus POM
    bool wall_displaced = false;
};
struct ResidentFlatFrame {
    std::uint64_t map_epoch = 0, device_epoch = 0, continuity = 0, frame_id = 0;
    std::uint32_t render_pass = 0, stage = 0, viewport_x = 0, viewport_y = 0;
    std::string map, map_sha256;
    FrameProvenance provenance;
    scene::Frame camera;
    std::vector<ResidentFlatDraw> draws;
    std::uint32_t override_instances = 0;
    std::uint32_t bedroom_material_runs = 0;
    bool bedroom_anchor_retained = false;
    bool bedroom_original_materials = false;
    std::uint32_t wall_source_triangles = 0, wall_displaced_triangles = 0;
    bool wall_displacement_failed = false;
};
// Build draw references without expanding or copying immutable geometry.
// Optional overrides affect only this consumer, never the source snapshot.
bool BuildResidentFlatFrame(const Snapshot& snapshot, ResidentFlatFrame& output,
                            const MeshOverride* bed_override = nullptr, bool bedroom_materials = false,
                            bool bedroom_wall_relief = false, const BedroomAnchor* retained_anchor = nullptr,
                            bool bedroom_originals = false);
// Expand a partial resident selection into the existing flat D3D12 input.
// Colors identify material IDs; they are not a lighting or material model.
// The output is a viewport-sized crop. This performs CPU expansion each frame;
// it is an integration step, not yet the resident GPU renderer.
bool BuildFlatFrame(const Snapshot& snapshot, scene::Frame& output);
} // namespace t3render::live
