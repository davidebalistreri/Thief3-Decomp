#pragma once

#include "scene_live_flat.hpp"
#include "scene_live_materials.hpp"

#include <vector>

namespace t3render::live {

struct WallDisplacementStats {
    std::uint32_t source_triangles=0, displaced_triangles=0;
};

// Viewer-only geometry. Original BSP resources and snapshot evidence are
// untouched; generated meshes are cached while their inputs remain current.
class WallDisplacementCache {
public:
    bool Apply(const Snapshot& snapshot, ResidentFlatFrame& frame,
               const std::shared_ptr<const BedroomMaterialLibrary>& materials, WallDisplacementStats& stats) noexcept;
    void Clear() noexcept;
private:
    struct Entry {
        std::shared_ptr<const Resource> source, replacement;
        std::uint32_t first=0, count=0;
    };
    std::vector<Entry> entries_;
    std::uint64_t map_=0,device_=0,continuity_=0,serial_=0;
    std::shared_ptr<const BedroomMaterialLibrary> materials_;
    Matrix bed_transform_{};
    std::size_t bytes_=0;
};

} // namespace t3render::live
