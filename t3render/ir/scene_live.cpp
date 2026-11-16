#include "scene_live.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace t3render::live {
namespace {
constexpr std::size_t kMaxId = 512, kMaxMaterials = 8192, kMaxResources = 4096;
constexpr std::size_t kMaxIdentities = 16384, kMaxSelected = 8192, kMaxSections = 1024;
constexpr std::size_t kMaxVertices = 1000000, kMaxIndices = 3000000;
constexpr std::size_t kMaxGeometryBytes = 512u * 1024u * 1024u;

bool Text(const std::string& value) {
    return !value.empty() && value.size() <= kMaxId && value.find('\0') == std::string::npos;
}
bool Id(const std::string& value) {
    if (!Text(value)) return false;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X') &&
        std::all_of(value.begin()+2, value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) return false;
    return true;
}
bool Digest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
template<std::size_t N> bool Finite(const std::array<float, N>& value) {
    return std::all_of(value.begin(), value.end(), [](float x) { return std::isfinite(x); });
}
template<class T> bool FiniteVectors(const std::vector<T>& values) {
    return std::all_of(values.begin(), values.end(), [](const T& value) { return Finite(value); });
}
bool SameSection(const Section& a, const Section& b) {
    return a.name == b.name && a.first_index == b.first_index && a.index_count == b.index_count &&
           a.material_id == b.material_id;
}
bool SameResource(const Resource& a, const Resource& b) {
    const auto& x = a.geometry; const auto& y = b.geometry;
    return a.id == b.id && a.name == b.name && a.kind == b.kind && a.source_sha256 == b.source_sha256 &&
        x.positions == y.positions && x.normals == y.normals && x.uv0 == y.uv0 && x.tangents == y.tangents &&
        x.indices == y.indices && x.sections.size() == y.sections.size() &&
        std::equal(x.sections.begin(), x.sections.end(), y.sections.begin(), SameSection);
}
bool ValidConventions(const Conventions& c) {
    if ((c.length_unit != LengthUnit::Unknown && c.length_unit != LengthUnit::T3Unit && c.length_unit != LengthUnit::Meter) ||
        (c.axes != Axes::Unknown && c.axes != Axes::T3Xyz) ||
        (c.up_axis != UpAxis::Unknown && c.up_axis != UpAxis::Z) ||
        (c.handedness != Handedness::Unknown && c.handedness != Handedness::Left && c.handedness != Handedness::Right) ||
        (c.winding != Winding::Unknown && c.winding != Winding::NormalAligned && c.winding != Winding::NormalOpposed) ||
        (c.uv_origin != UvOrigin::Unknown && c.uv_origin != UvOrigin::TopLeft && c.uv_origin != UvOrigin::BottomLeft) ||
        (c.clip_depth != ClipDepth::Unknown && c.clip_depth != ClipDepth::ZeroToOne && c.clip_depth != ClipDepth::MinusOneToOne))
        return false;
    return !c.meters_per_unit || (std::isfinite(*c.meters_per_unit) && *c.meters_per_unit > 0 &&
        c.length_unit != LengthUnit::Unknown && (c.length_unit != LengthUnit::Meter || *c.meters_per_unit == 1));
}
bool SameConventions(const Conventions& a, const Conventions& b) {
    return a.length_unit == b.length_unit && a.meters_per_unit == b.meters_per_unit && a.axes == b.axes &&
        a.up_axis == b.up_axis && a.handedness == b.handedness && a.winding == b.winding &&
        a.uv_origin == b.uv_origin && a.clip_depth == b.clip_depth;
}
bool ValidProvenance(const FrameProvenance& provenance) {
    if (!Id(provenance.session_id)) return false;
    if (provenance.origin == FrameOrigin::CapturedReplay)
        return provenance.capture_sha256 && Digest(*provenance.capture_sha256);
    return (provenance.origin == FrameOrigin::LiveProcess || provenance.origin == FrameOrigin::Synthetic) &&
           !provenance.capture_sha256;
}
bool SameProvenance(const FrameProvenance& a, const FrameProvenance& b) {
    return a.origin == b.origin && a.session_id == b.session_id && a.capture_sha256 == b.capture_sha256;
}
bool ValidCamera(const Camera& camera) {
    return Finite(camera.view) && Finite(camera.projection) && camera.width && camera.height &&
        camera.x <= UINT32_MAX-camera.width && camera.y <= UINT32_MAX-camera.height &&
        std::isfinite(camera.min_depth) && std::isfinite(camera.max_depth) && camera.min_depth <= camera.max_depth;
}
bool ValidEvidence(BindingEvidence evidence) {
    return evidence == BindingEvidence::Verified || evidence == BindingEvidence::Candidate;
}
bool ValidSections(const std::vector<std::uint32_t>& sections, std::size_t available) {
    if (sections.empty() || sections.size() > kMaxSections) return false;
    std::unordered_set<std::uint32_t> seen;
    for (auto section : sections) if (section >= available || !seen.insert(section).second) return false;
    return true;
}
} // namespace

bool SceneResidency::BeginMap(std::uint64_t expected_map_epoch, const std::string& map, const std::string& map_sha256) {
    if (!Text(map) || !Digest(map_sha256)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (resetting_ || expected_map_epoch != map_epoch_ || map_epoch_ == UINT64_MAX || device_epoch_ == UINT64_MAX) return false;
    // Copy first, so allocation failure cannot leave a half-transitioned map.
    auto next_map = map, next_hash = map_sha256;
    map_.swap(next_map); map_sha256_.swap(next_hash);
    ++map_epoch_; ++device_epoch_;
    materials_.clear(); resources_.clear(); identities_.clear(); geometry_bytes_ = 0;
    snapshot_.reset(); provenance_.reset(); conventions_.reset(); has_frame_ = false; last_frame_ = 0;
    return true;
}

std::uint64_t SceneResidency::BeginReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (resetting_ || device_epoch_ == UINT64_MAX) return false;
    ++device_epoch_; resetting_ = true; ready_ = false; snapshot_.reset();
    return device_epoch_;
}

bool SceneResidency::EndReset(std::uint64_t ticket, bool succeeded) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resetting_ || !ticket || ticket != device_epoch_) return false;
    resetting_ = false; ready_ = succeeded;
    return true;
}

bool SceneResidency::AddMaterial(std::uint64_t map_epoch, Material material) {
    if (!Id(material.id) || !Text(material.name)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!map_epoch_ || map_epoch != map_epoch_ || resetting_) return false;
    const auto item = materials_.find(material.id);
    if (item != materials_.end()) return item->second.name == material.name;
    if (materials_.size() >= kMaxMaterials) return false;
    const auto key = material.id;
    return materials_.emplace(key, std::move(material)).second;
}

bool SceneResidency::AddResource(std::uint64_t map_epoch, Resource resource) {
    const auto& g = resource.geometry;
    if (!Id(resource.id) || !Text(resource.name) || !Digest(resource.source_sha256) ||
        (resource.kind != ResourceKind::StaticMesh && resource.kind != ResourceKind::BspBlock) ||
        g.positions.empty() || g.positions.size() > kMaxVertices || g.indices.empty() ||
        g.indices.size() > kMaxIndices || g.sections.empty() || g.sections.size() > kMaxSections ||
        (!g.normals.empty() && g.normals.size() != g.positions.size()) ||
        (!g.uv0.empty() && g.uv0.size() != g.positions.size()) ||
        (!g.tangents.empty() && g.tangents.size() != g.positions.size()) ||
        !FiniteVectors(g.positions) || !FiniteVectors(g.normals) || !FiniteVectors(g.uv0) ||
        !FiniteVectors(g.tangents)) return false;
    for (auto index : g.indices) if (index >= g.positions.size()) return false;
    for (const auto& section : g.sections) {
        if (!Text(section.name) || !section.index_count || section.index_count % 3 ||
            std::uint64_t{section.first_index} + section.index_count > g.indices.size() ||
            (section.material_id && !Id(*section.material_id))) return false;
    }
    const std::size_t bytes = (g.positions.size()+g.normals.size()+g.tangents.size())*sizeof(Vec3) +
                              g.uv0.size()*sizeof(Vec2) + g.indices.size()*sizeof(std::uint32_t);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!map_epoch_ || map_epoch != map_epoch_ || resetting_) return false;
    for (const auto& section : g.sections)
        if (section.material_id && materials_.find(*section.material_id) == materials_.end()) return false;
    const auto item = resources_.find(resource.id);
    if (item != resources_.end()) return SameResource(*item->second, resource);
    if (resources_.size() >= kMaxResources || bytes > kMaxGeometryBytes-geometry_bytes_) return false;
    const auto key = resource.id;
    auto immutable = std::make_shared<const Resource>(std::move(resource));
    if (!resources_.emplace(key, std::move(immutable)).second) return false;
    geometry_bytes_ += bytes;
    return true;
}

bool SceneResidency::Publish(const FrameInput& frame) {
    if (!frame.synchronized || !ValidProvenance(frame.provenance) || !ValidConventions(frame.conventions) ||
        !ValidCamera(frame.camera) ||
        frame.meshes.size() > kMaxSelected || frame.bsp.size() > kMaxSelected ||
        frame.meshes.size() + frame.bsp.size() > kMaxSelected) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!map_epoch_ || !ready_ || resetting_ || frame.map_epoch != map_epoch_ ||
        frame.device_epoch != device_epoch_ || (has_frame_ && frame.frame_id <= last_frame_) ||
        (provenance_ && !SameProvenance(*provenance_, frame.provenance)) ||
        (conventions_ && !SameConventions(*conventions_, frame.conventions))) return false;
    auto next = std::make_shared<Snapshot>();
    next->map = map_; next->map_sha256 = map_sha256_;
    next->map_epoch = map_epoch_; next->device_epoch = device_epoch_; next->frame_id = frame.frame_id;
    next->render_pass = frame.render_pass; next->stage = frame.stage; next->camera = frame.camera;
    next->provenance = frame.provenance; next->conventions = frame.conventions;
    next->materials = materials_;
    next->meshes.reserve(frame.meshes.size()); next->bsp.reserve(frame.bsp.size());
    auto identities = identities_; // committed only after the entire frame validates
    std::unordered_set<std::string> mesh_ids, bsp_ids;
    for (const auto& mesh : frame.meshes) {
        const auto resource = resources_.find(mesh.resource_id);
        if (!Id(mesh.instance_id) || !mesh.generation || !ValidEvidence(mesh.binding) || !Finite(mesh.transform) ||
            !mesh_ids.insert(mesh.instance_id).second || resource == resources_.end() ||
            resource->second->kind != ResourceKind::StaticMesh ||
            !ValidSections(mesh.sections, resource->second->geometry.sections.size()) ||
            (!mesh.material_ids.empty() && mesh.material_ids.size() != resource->second->geometry.sections.size())) return false;
        for (const auto& id : mesh.material_ids) if (id && materials_.find(*id) == materials_.end()) return false;
        const auto identity = identities.find(mesh.instance_id);
        if (identity != identities.end()) {
            if (mesh.generation < identity->second.generation ||
                (mesh.generation == identity->second.generation && mesh.resource_id != identity->second.resource_id)) return false;
            if (mesh.generation > identity->second.generation) identity->second = {mesh.generation, mesh.resource_id};
        } else {
            if (identities.size() >= kMaxIdentities) return false;
            identities.emplace(mesh.instance_id, Identity{mesh.generation, mesh.resource_id});
        }
        next->meshes.push_back({mesh, resource->second});
    }
    for (const auto& block : frame.bsp) {
        const auto resource = resources_.find(block.resource_id);
        if (!ValidEvidence(block.binding) || !bsp_ids.insert(block.resource_id).second || resource == resources_.end() ||
            resource->second->kind != ResourceKind::BspBlock ||
            !ValidSections(block.sections, resource->second->geometry.sections.size())) return false;
        next->bsp.push_back({block, resource->second});
    }
    // All allocations are completed before mutating the published state.
    auto provenance = frame.provenance;
    identities_.swap(identities); provenance_ = std::move(provenance); conventions_ = frame.conventions;
    snapshot_ = std::move(next); last_frame_ = frame.frame_id; has_frame_ = true;
    return true;
}

bool SceneResidency::DiscardFrame(std::uint64_t map_epoch, std::uint64_t device_epoch, std::uint64_t through_frame_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!map_epoch_ || map_epoch != map_epoch_ || device_epoch != device_epoch_) return false;
    if (snapshot_ && snapshot_->frame_id > through_frame_id) return false;
    snapshot_.reset();
    return true;
}

Epochs SceneResidency::epochs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {map_epoch_, device_epoch_, ready_ && !resetting_};
}
std::shared_ptr<const Snapshot> SceneResidency::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

} // namespace t3render::live
