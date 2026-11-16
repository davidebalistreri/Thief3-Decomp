#pragma once

#include "../ir/scene_live.hpp"
#include "scene_packet.hpp"

namespace t3render::live {
struct ResidentFlatDraw {
    std::shared_ptr<const Resource> resource;
    std::uint32_t section = 0;
    Matrix transform{};
    std::array<float,4> color{};
};
struct ResidentFlatFrame {
    std::uint64_t map_epoch = 0, device_epoch = 0, continuity = 0;
    scene::Frame camera;
    std::vector<ResidentFlatDraw> draws;
};
// Build draw references without expanding or copying immutable geometry.
bool BuildResidentFlatFrame(const Snapshot& snapshot, ResidentFlatFrame& output);
// Expand a partial resident selection into the existing flat D3D12 input.
// Colors identify material IDs; they are not a lighting or material model.
// The output is a viewport-sized crop. This performs CPU expansion each frame;
// it is an integration step, not yet the resident GPU renderer.
bool BuildFlatFrame(const Snapshot& snapshot, scene::Frame& output);
} // namespace t3render::live
