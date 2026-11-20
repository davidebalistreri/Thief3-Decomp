#pragma once

#include "scene_live_catalog.hpp"
#include "scene_live_light_raw.hpp"
#include <istream>

namespace t3render::live {
// Portable, bounded description of a frozen selection. The catalog owns the
// asset-derived geometry and materials. The two light batches are separately
// observed candidates, not synchronized live observations of this camera.
struct BenchmarkScene {
    std::shared_ptr<const Snapshot> on, off;
    RawLightFrame on_lights, off_lights;
};

// Consumes a newly loaded catalog. On failure the output is unchanged.
bool LoadBenchmarkScene(std::istream& input, LoadedCatalog& catalog, BenchmarkScene& output);

// Overlay matching live transforms onto the frozen selection. Objects absent
// from this partial live frame retain their recorded transforms. Both light
// states and the camera remain the independently captured benchmark values.
bool BenchmarkFromLive(const BenchmarkScene& golden, const Snapshot& live,
                       BenchmarkScene& output, std::size_t& matched_meshes, std::size_t& matched_bsp);
} // namespace t3render::live
