#pragma once

#include "../ir/scene_live.hpp"
#include "scene_packet.hpp"

namespace t3render::live {
// Expand a partial resident selection into the existing flat D3D12 input.
// Colors identify material IDs; they are not a lighting or material model.
// The output is a viewport-sized crop. This performs CPU expansion each frame;
// it is an integration step, not yet the resident GPU renderer.
bool BuildFlatFrame(const Snapshot& snapshot, scene::Frame& output);
} // namespace t3render::live
