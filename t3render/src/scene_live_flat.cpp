#include "scene_live_flat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace t3render::live {
namespace {
const Matrix kIdentity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

bool AddSection(scene::Frame& frame, const Resource& resource, const Matrix& matrix,
                std::uint32_t index, const std::optional<std::string>& material) {
    if (!std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); }) ||
        index >= resource.geometry.sections.size() || frame.draws.size() >= scene::kMaxDraws ||
        std::fabs(matrix[3]) > 1e-6f || std::fabs(matrix[7]) > 1e-6f ||
        std::fabs(matrix[11]) > 1e-6f || std::fabs(matrix[15]-1) > 1e-6f) return false;
    const auto& geometry = resource.geometry;
    const auto& section = geometry.sections[index];
    if (!section.index_count || section.index_count % 3 ||
        std::uint64_t{section.first_index} + section.index_count > geometry.indices.size() ||
        section.index_count > scene::kMaxTriangles*3 - frame.vertices.size()) return false;
    scene::Draw draw{};
    draw.texture = 0; draw.first_vertex = static_cast<std::uint32_t>(frame.vertices.size());
    draw.vertex_count = section.index_count;
    draw.state.cull = 0; // absolute T3 winding is not established
    draw.shading = 1;
    std::uint32_t hash = 2166136261u;
    const std::string key = material ? *material : "unknown";
    for (const auto c : key) { hash ^= static_cast<unsigned char>(c); hash *= 16777619u; }
    for (unsigned i = 0; i < 3; ++i) draw.ambient_color[i] = (64 + ((hash >> (i*8)) & 127u)) / 255.0f;
    draw.ambient_color[3] = 1;
    for (std::uint32_t n = 0; n < section.index_count; ++n) {
        const auto vertex_index = geometry.indices[section.first_index+n];
        if (vertex_index >= geometry.positions.size()) return false;
        const auto& p = geometry.positions[vertex_index];
        float world[3]{};
        for (unsigned j = 0; j < 3; ++j) {
            const double v = double(p[0])*matrix[j] + double(p[1])*matrix[4+j] +
                             double(p[2])*matrix[8+j] + matrix[12+j];
            if (!std::isfinite(v) || std::fabs(v) > 1000000) return false;
            world[j] = static_cast<float>(v);
        }
        scene::Vertex vertex{};
        vertex.x = world[0]; vertex.y = world[1]; vertex.z = world[2]; vertex.nz = 1;
        frame.vertices.push_back(vertex);
    }
    frame.draws.push_back(draw);
    return true;
}
} // namespace

bool BuildFlatFrame(const Snapshot& snapshot, scene::Frame& output) {
    const auto& camera = snapshot.camera;
    if (snapshot.conventions.length_unit != LengthUnit::T3Unit || snapshot.conventions.axes != Axes::T3Xyz ||
        snapshot.conventions.up_axis != UpAxis::Z || snapshot.conventions.clip_depth == ClipDepth::MinusOneToOne ||
        !std::all_of(camera.view.begin(), camera.view.end(), [](float v) { return std::isfinite(v); }) ||
        !std::all_of(camera.projection.begin(), camera.projection.end(), [](float v) { return std::isfinite(v); }) ||
        !std::isfinite(camera.min_depth) || !std::isfinite(camera.max_depth) ||
        !camera.width || !camera.height || camera.width > 2048 || camera.height > 2048 ||
        camera.min_depth < 0 || camera.max_depth > 1 || camera.min_depth > camera.max_depth) return false;
    scene::Frame frame;
    frame.width = camera.width; frame.height = camera.height;
    frame.min_depth = camera.min_depth; frame.max_depth = camera.max_depth;
    std::copy(camera.view.begin(), camera.view.end(), frame.view);
    std::copy(camera.projection.begin(), camera.projection.end(), frame.projection);
    scene::Texture texture;
    texture.mips.push_back({1, 1, {255,255,255,255}});
    frame.textures.push_back(std::move(texture));
    for (const auto& mesh : snapshot.meshes) {
        if (!mesh.resource) return false;
        const auto& selection = mesh.selection;
        if (!selection.material_ids.empty() &&
            selection.material_ids.size() != mesh.resource->geometry.sections.size()) return false;
        for (auto index : selection.sections) {
            if (index >= mesh.resource->geometry.sections.size()) return false;
            const auto material = selection.material_ids.empty() ? mesh.resource->geometry.sections[index].material_id
                                                                 : selection.material_ids[index];
            if (!AddSection(frame, *mesh.resource, selection.transform, index, material)) return false;
        }
    }
    for (const auto& block : snapshot.bsp) {
        if (!block.resource) return false;
        for (auto index : block.selection.sections) {
            if (index >= block.resource->geometry.sections.size() ||
                !AddSection(frame, *block.resource, kIdentity, index,
                            block.resource->geometry.sections[index].material_id)) return false;
        }
    }
    output = std::move(frame);
    return true;
}
} // namespace t3render::live
