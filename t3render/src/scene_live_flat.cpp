#include "scene_live_flat.hpp"
#include "scene_live_materials.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace t3render::live {
namespace {
const Matrix kIdentity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
std::array<float,4> MaterialColor(const std::optional<std::string>& material) {
    std::uint32_t hash = 2166136261u;
    const std::string key = material ? *material : "unknown";
    for (const auto c : key) { hash ^= static_cast<unsigned char>(c); hash *= 16777619u; }
    std::array<float,4> color{};
    for (unsigned i = 0; i < 3; ++i) color[i] = (64 + ((hash >> (i*8)) & 127u)) / 255.0f;
    color[3] = 1;return color;
}

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
    const auto color=MaterialColor(material);
    std::copy(color.begin(),color.end(),draw.ambient_color);
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

bool BuildResidentFlatFrame(const Snapshot& snapshot, ResidentFlatFrame& output, const MeshOverride* bed_override,
                            bool bedroom_materials, bool bedroom_wall_relief) {
    const auto& camera=snapshot.camera;
    if (!snapshot.map_epoch || !snapshot.device_epoch || !snapshot.continuity || snapshot.conventions.length_unit!=LengthUnit::T3Unit ||
        snapshot.conventions.axes!=Axes::T3Xyz || snapshot.conventions.up_axis!=UpAxis::Z ||
        snapshot.conventions.clip_depth==ClipDepth::MinusOneToOne ||
        !std::all_of(camera.view.begin(),camera.view.end(),[](float v){return std::isfinite(v);}) ||
        !std::all_of(camera.projection.begin(),camera.projection.end(),[](float v){return std::isfinite(v);}) ||
        !std::isfinite(camera.min_depth) || !std::isfinite(camera.max_depth) || !camera.width || !camera.height ||
        camera.width>2048 || camera.height>2048 || camera.min_depth<0 || camera.max_depth>1 || camera.min_depth>camera.max_depth) return false;
    ResidentFlatFrame frame;frame.map_epoch=snapshot.map_epoch;frame.device_epoch=snapshot.device_epoch;
    frame.continuity=snapshot.continuity;frame.frame_id=snapshot.frame_id;
    frame.render_pass=snapshot.render_pass;frame.stage=snapshot.stage;frame.map=snapshot.map;
    frame.map_sha256=snapshot.map_sha256;frame.provenance=snapshot.provenance;
    frame.viewport_x=camera.x;frame.viewport_y=camera.y;
    frame.camera.width=camera.width;frame.camera.height=camera.height;
    frame.camera.min_depth=camera.min_depth;frame.camera.max_depth=camera.max_depth;
    std::copy(camera.view.begin(),camera.view.end(),frame.camera.view);
    std::copy(camera.projection.begin(),camera.projection.end(),frame.camera.projection);
    std::size_t indices=0;
    auto append=[&](const std::shared_ptr<const Resource>& resource,const Matrix& transform,
                    std::uint32_t section,std::uint32_t first,std::uint32_t count,
                    const std::optional<std::string>& material) {
        if (!resource || section>=resource->geometry.sections.size() || frame.draws.size()>=scene::kMaxDraws ||
            !std::all_of(transform.begin(),transform.end(),[](float v){return std::isfinite(v);}) ||
            std::fabs(transform[3])>1e-6f || std::fabs(transform[7])>1e-6f ||
            std::fabs(transform[11])>1e-6f || std::fabs(transform[15]-1)>1e-6f) return false;
        const auto& part=resource->geometry.sections[section];
        if (!part.index_count || part.index_count%3 || !count || count%3 || first<part.first_index ||
            (first-part.first_index)%3 || std::uint64_t{first}+count>std::uint64_t{part.first_index}+part.index_count ||
            std::uint64_t{part.first_index}+part.index_count>resource->geometry.indices.size() ||
            count>scene::kMaxTriangles*3-indices) return false;
        indices+=count;
        frame.draws.push_back({resource,section,first,count,transform,material,MaterialColor(material)});return true;
    };
    for (const auto& mesh:snapshot.meshes) {
        if (!mesh.resource || (!mesh.selection.material_ids.empty() &&
            mesh.selection.material_ids.size()!=mesh.resource->geometry.sections.size())) return false;
        for (auto index:mesh.selection.sections)
            if (index>=mesh.resource->geometry.sections.size()) return false;
        if (bed_override && snapshot.map==bed_override->map && snapshot.map_sha256==bed_override->map_sha256 &&
            mesh.selection.instance_id==bed_override->instance_id && !mesh.selection.sections.empty()) {
            if (!bed_override->source || !bed_override->replacement || mesh.resource!=bed_override->source ||
                mesh.selection.resource_id!=bed_override->source->id ||
                bed_override->replacement->kind!=ResourceKind::StaticMesh ||
                bed_override->replacement->id.empty() || bed_override->replacement->id==bed_override->source->id ||
                bed_override->replacement->geometry.sections.empty() ||
                bed_override->replacement->geometry.sections.size()>scene::kMaxDraws) return false;
            for (std::size_t i=0;i<bed_override->replacement->geometry.sections.size();++i) {
                const auto& part=bed_override->replacement->geometry.sections[i];
                if (!append(bed_override->replacement,mesh.selection.transform,static_cast<std::uint32_t>(i),
                            part.first_index,part.index_count,part.material_id)) return false;
            }
            ++frame.override_instances;continue;
        }
        for (auto index:mesh.selection.sections) {
            const auto material=mesh.selection.material_ids.empty() ? mesh.resource->geometry.sections[index].material_id :
                                                                       mesh.selection.material_ids[index];
            const auto& part=mesh.resource->geometry.sections[index];
            if (!append(mesh.resource,mesh.selection.transform,index,part.first_index,part.index_count,material)) return false;
        }
    }
    BedroomAnchor anchor;
    const bool have_anchor=bedroom_materials && FindBedroomAnchor(snapshot,anchor);
    const bool identify_head_wall=have_anchor && bedroom_wall_relief && snapshot.map_sha256==
        "ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
    for (const auto& block:snapshot.bsp) {
        if (!block.resource) return false;
        for (auto index:block.selection.sections) {
            if (index>=block.resource->geometry.sections.size()) return false;
            const auto& part=block.resource->geometry.sections[index];
            const auto found=part.material_id ? snapshot.materials.find(*part.material_id) : snapshot.materials.end();
            if (have_anchor && found!=snapshot.materials.end()) {
                std::vector<BedroomMaterialRun> runs;
                if (!PartitionBedroomMaterial(anchor,*block.resource,index,found->second.name,runs,identify_head_wall)) return false;
                for (const auto& run:runs) {
                    if (!append(block.resource,kIdentity,index,run.first_index,run.index_count,
                                run.replacement ? run.replacement : part.material_id)) return false;
                    if (run.head_wall) frame.draws.back().wall_relief=2;
                    if (run.replacement) ++frame.bedroom_material_runs;
                }
            } else if (!append(block.resource,kIdentity,index,part.first_index,part.index_count,part.material_id)) return false;
        }
    }
    output=std::move(frame);return true;
}
} // namespace t3render::live
