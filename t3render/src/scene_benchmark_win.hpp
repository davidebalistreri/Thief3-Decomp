#pragma once
#include "scene_benchmark.hpp"
#include <string>

namespace t3render::live {
struct BenchmarkPackage {
    BenchmarkScene scene;
    std::shared_ptr<const MeshOverride> bed;
    std::shared_ptr<const BedroomMaterialLibrary> materials;
    std::vector<std::uint8_t> original_rgb;
    std::string catalog_sha256,scene_sha256,original_sha256;
};
struct BenchmarkImages {
    std::vector<std::uint8_t> pixels[3]; // tightly packed RGBA, flat/on/off
    double cpu_readback_ms[3]{};
    std::uint32_t draws[3]{},triangles[3]{},shadowed_lights[3]{};
};
bool LoadBenchmarkPackage(const std::wstring& directory,BenchmarkPackage& output) noexcept;
bool RenderBenchmark(const BenchmarkPackage& package,const BenchmarkScene& scene,BenchmarkImages& output) noexcept;
// The caller creates a new output directory. The result marker is written last.
bool WriteBenchmarkImages(const std::wstring& directory,const BenchmarkPackage& package,
                          const BenchmarkScene& scene,const Snapshot& live,
                          std::size_t meshes,std::size_t bsp,const BenchmarkImages& images) noexcept;
} // namespace t3render::live
