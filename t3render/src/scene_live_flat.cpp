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
                            bool bedroom_materials, bool bedroom_wall_relief, const BedroomAnchor* retained_anchor,
                            bool bedroom_originals) {
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
    BedroomAnchor anchor;
    bool have_anchor=bedroom_materials && FindBedroomAnchor(snapshot,anchor);
    if (bedroom_materials && !have_anchor && retained_anchor) {
        anchor=*retained_anchor;have_anchor=true;frame.bedroom_anchor_retained=true;
    }
    const std::string actor_prefix="actor:"+snapshot.map+":"+snapshot.map_sha256+":";
    const bool bedroom_preview=bed_override && have_anchor && snapshot.map=="SouthQuarter1_int1" &&
        snapshot.map_sha256=="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba" &&
        bed_override->map==snapshot.map && bed_override->map_sha256==snapshot.map_sha256 &&
        bed_override->instance_id==actor_prefix+"StaticMeshActor__1520" && bed_override->source && bed_override->replacement;
    frame.bedroom_original_materials=bedroom_preview;
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
                auto material=part.material_id;
                // Match the existing bedroom PBR preview: the generated bed's
                // two wood sections use the same wood_floor_v2 texture. Keep
                // this a viewer-only choice, and only when that library is on.
                if (bedroom_preview && material &&
                    (*material=="remaster:material:bed_high:BedTrimWood" ||
                     *material=="remaster:material:bed_high:HeadboardWood"))
                    material="remaster:material:wood_floor_v2";
                else if (bedroom_preview && material && *material=="remaster:material:bed_high:GENiFABRICwoolA2_D")
                    material="remaster:material:bed_high:textured:GENiFABRICwoolA2_D";
                else if (bedroom_preview && material && *material=="remaster:material:bed_high:PillowLinen")
                    material="remaster:material:bed_high:textured:PillowLinen";
                if (!append(bed_override->replacement,mesh.selection.transform,static_cast<std::uint32_t>(i),
                            part.first_index,part.index_count,material)) return false;
                // These colors are the fallback for catalogs without the two
                // optional bed textile textures. Textured draws ignore them.
                if (bedroom_preview && part.material_id) {
                    const auto& id=*part.material_id;
                    if (id=="remaster:material:bed_high:GENiFABRICwoolA2_D") frame.draws.back().color={.13f,.15f,.25f,1};
                    else if (id=="remaster:material:bed_high:BlanketSeam") frame.draws.back().color={.15f,.14f,.16f,1};
                    else if (id=="remaster:material:bed_high:PillowLinen") frame.draws.back().color={.75f,.70f,.60f,1};
                    else if (id=="remaster:material:bed_high:PillowSeam") frame.draws.back().color={.46f,.44f,.40f,1};
                }
            }
            ++frame.override_instances;continue;
        }
        for (auto index:mesh.selection.sections) {
            auto material=mesh.selection.material_ids.empty() ? mesh.resource->geometry.sections[index].material_id :
                                                                 mesh.selection.material_ids[index];
            const auto& part=mesh.resource->geometry.sections[index];
            std::optional<std::array<float,4>> preview_color;
            if (bedroom_preview && material && mesh.selection.instance_id.compare(0,actor_prefix.size(),actor_prefix)==0) {
                const auto found=snapshot.materials.find(*material);
                const auto actor=mesh.selection.instance_id.substr(actor_prefix.size());
                const auto& name=mesh.resource->name;
                const auto source=found==snapshot.materials.end() ? std::string{} : found->second.name;
                const bool has_uv=mesh.resource->geometry.uv0.size()==mesh.resource->geometry.positions.size();
                const bool furniture=(actor=="D_443__1" && name=="SEAinFURNcabinetC1") ||
                                     ((actor=="D_1224__21" || actor=="D_1224__0" ||
                                       actor=="D_1224__22" || actor=="D_1224__23") && name=="GENxGOODScrateB");
                static constexpr const char* beam_actors[]{"StaticMeshActor__2","StaticMeshActor__16","StaticMeshActor__17",
                    "StaticMeshActor__19","StaticMeshActor__56","StaticMeshActor__258","StaticMeshActor__603",
                    "StaticMeshActor__604","StaticMeshActor__610","StaticMeshActor__1037","StaticMeshActor__1306",
                    "StaticMeshActor__1337","StaticMeshActor__1363","StaticMeshActor__1365","StaticMeshActor__1409",
                    "StaticMeshActor__1456","StaticMeshActor__2860"};
                const bool beam=std::any_of(std::begin(beam_actors),std::end(beam_actors),[&](const char* value){return actor==value;}) &&
                    (name=="Rsupportwood" || name=="Tenxbeamwood1x8" || name=="Tenxbeamwood1x16" ||
                     name=="TENxBEAMwood1x5" || name=="GENrafters1") &&
                    (source=="GENbeamA_D" || source=="GENgoodnessA_D");
                const bool target=actor=="D_548__0" && name=="GENxGOODSarchtarg";
                // Frame 889 also selects the matching arch at the room's
                // right edge. Keep this mapping tied to the two known actors.
                const bool arch=(actor=="StaticMeshActor__58" || actor=="StaticMeshActor__59") &&
                                name=="GENsknDOORFRMarc4x10half";
                if (has_uv && !bedroom_originals && ((furniture && source=="HSEGoodnessA_D") || beam || (target && source=="GENgoodnessA_D")))
                    material="remaster:material:wood_floor_v2";
                else if (has_uv && ((arch && source=="STMwall512D_D") ||
                         (actor=="StaticMeshActor__239" && name=="GENwindowFACADE2A1" && source=="CHRwallC_D")))
                    material="remaster:material:stone_wall_v2";
                else if (arch && source=="CIVGoodness_D")
                    preview_color=std::array<float,4>{.30f,.28f,.24f,1};
                else if (actor=="StaticMeshActor__1" && name=="GENiGOODSbooksA3" && source=="GENgoodnessBOOK1_D")
                    preview_color=std::array<float,4>{.32f,.20f,.12f,1};
                else if (actor=="D_125__3" && name=="GENiFOODbreadA1" && source=="GENbreadcrust_D")
                    preview_color=std::array<float,4>{.62f,.38f,.19f,1};
                else if (target && source=="GENtarget_D") preview_color=std::array<float,4>{.60f,.15f,.08f,1};
            }
            if (!append(mesh.resource,mesh.selection.transform,index,part.first_index,part.index_count,material)) return false;
            if (preview_color) frame.draws.back().color=*preview_color;
        }
    }
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
                    // The wall visible through the bedroom doorway was
                    // submitted in the captured scope, outside the original
                    // bed-relative room box. Keep this one block explicit.
                    const bool doorway_wall=bedroom_preview &&
                        block.resource->id=="bsp:SouthQuarter1_int1:ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba:19:190995" &&
                        block.resource->geometry.uv0.size()==block.resource->geometry.positions.size() &&
                        found->second.name=="STMwall512D_D";
                    const auto material=doorway_wall ? std::optional<std::string>("remaster:material:stone_wall_v2") :
                                                       run.replacement ? run.replacement : part.material_id;
                    if (!append(block.resource,kIdentity,index,run.first_index,run.index_count,
                                material)) return false;
                    if (run.head_wall) frame.draws.back().wall_relief=2;
                    if (run.replacement || doorway_wall) ++frame.bedroom_material_runs;
                }
            } else if (!append(block.resource,kIdentity,index,part.first_index,part.index_count,part.material_id)) return false;
        }
    }
    output=std::move(frame);return true;
}
} // namespace t3render::live
