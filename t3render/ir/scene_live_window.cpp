#include "scene_live_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace t3render::live {
namespace {
bool SameCamera(const ProbeCamera& a, const ProbeCamera& b) {
    return std::memcmp(a.view.data(), b.view.data(), sizeof(float)*a.view.size()) == 0 &&
           std::memcmp(a.projection.data(), b.projection.data(), sizeof(float)*a.projection.size()) == 0;
}
bool ValidCamera(const ProbeCamera& camera) {
    const auto finite = [](float x) { return std::isfinite(x); };
    return std::all_of(camera.view.begin(), camera.view.end(), finite) &&
           std::all_of(camera.projection.begin(), camera.projection.end(), finite);
}
bool ValidViewport(const Viewport& v) {
    return v.width && v.height && v.x <= UINT32_MAX-v.width && v.y <= UINT32_MAX-v.height &&
        std::isfinite(v.min_depth) && std::isfinite(v.max_depth) && v.min_depth <= v.max_depth;
}
bool SameViewport(const Viewport& a, const Viewport& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
           std::memcmp(&a.min_depth, &b.min_depth, sizeof(float)) == 0 &&
           std::memcmp(&a.max_depth, &b.max_depth, sizeof(float)) == 0;
}
} // namespace

SceneWindow::SceneWindow(SceneResidency& residency, Epochs epochs, FrameProvenance provenance,
                         Conventions conventions, std::uint32_t render_pass, std::uint32_t stage)
    : residency_(residency), epochs_(epochs), provenance_(std::move(provenance)),
      conventions_(conventions), pass_(render_pass), stage_(stage) {}

void SceneWindow::RequireRebind() {
    tainted_ = true; rebind_required_ = true;
    if (present_ticket_) boundary_ambiguous_ = true;
    (void)residency_.DiscardFrame(epochs_.map, epochs_.device, present_ticket_ ? present_frame_ : previous_frame_);
}
std::uint64_t SceneWindow::NextTicket() {
    if (!next_ticket_ || next_ticket_ == UINT64_MAX) { RequireRebind(); return 0; }
    return next_ticket_++;
}
bool SceneWindow::SameThread(ThreadToken a, ThreadToken b) const {
    return a.id && a.birth && a.id == b.id && a.birth == b.birth;
}
bool SceneWindow::Eligible(ThreadToken thread) {
    if (!primed_ || rebind_required_) return false;
    if (present_ticket_ || !SameThread(thread, previous_thread_)) { RequireRebind(); return false; }
    return !tainted_;
}
void SceneWindow::ClearInterval() {
    camera_.reset(); viewport_.reset(); mesh_.reset(); bsp_.reset();
    meshes_.clear(); blocks_.clear(); mesh_index_.clear(); block_index_.clear();
    probe_ticket_ = scope_ticket_ = draw_ticket_ = 0;
    probe_thread_ = scope_thread_ = draw_thread_ = {};
    scope_draws_ = 0; draw_scoped_ = false; draw_scope_ticket_ = 0; tainted_ = false; active_probe_matches_ = false;
}

std::uint64_t SceneWindow::BeginPresent(ThreadToken thread, std::uint64_t frame_id, bool adapter_quiescent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rebind_required_) return 0;
    if (!thread.id || !thread.birth || present_ticket_ || !adapter_quiescent) {
        RequireRebind(); return 0;
    }
    const auto ticket = NextTicket();
    if (!ticket) return 0;
    present_ticket_ = ticket; present_thread_ = thread; present_frame_ = frame_id;
    if (probe_ticket_ || scope_ticket_ || draw_ticket_) RequireRebind();
    if (primed_ && (previous_frame_ == UINT64_MAX || frame_id != previous_frame_+1 ||
                    !SameThread(thread, previous_thread_))) tainted_ = true;
    return ticket;
}

WindowResult SceneWindow::EndPresent(std::uint64_t ticket, ThreadToken thread, bool succeeded) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ticket || ticket != present_ticket_ || !SameThread(thread, present_thread_)) {
        RequireRebind(); return WindowResult::Rejected;
    }
    const auto actual = residency_.epochs();
    const bool epoch_valid = epochs_.ready && actual.ready && actual.map == epochs_.map && actual.device == epochs_.device;
    if (!epoch_valid) RequireRebind();
    const bool no_pending = !probe_ticket_ && !scope_ticket_ && !draw_ticket_;
    const bool was_primed = primed_;
    bool published = false;
    if (primed_ && succeeded && epoch_valid && !tainted_ && !boundary_ambiguous_ && no_pending && camera_ && viewport_) {
        FrameInput frame;
        frame.map_epoch = epochs_.map; frame.device_epoch = epochs_.device; frame.frame_id = present_frame_;
        frame.render_pass = pass_; frame.stage = stage_; frame.provenance = provenance_; frame.conventions = conventions_;
        frame.synchronized = true;
        frame.camera.view = camera_->view; frame.camera.projection = camera_->projection;
        frame.camera.x = viewport_->x; frame.camera.y = viewport_->y;
        frame.camera.width = viewport_->width; frame.camera.height = viewport_->height;
        frame.camera.min_depth = viewport_->min_depth; frame.camera.max_depth = viewport_->max_depth;
        frame.meshes = meshes_; frame.bsp = blocks_;
        published = residency_.Publish(frame);
    }
    if (!published) (void)residency_.DiscardFrame(epochs_.map, epochs_.device, present_frame_);
    // A complete successful boundary may prime a later interval even when the
    // interval just ended was invalid. An overlapping Present cannot.
    const bool can_prime = succeeded && epoch_valid && !boundary_ambiguous_ && !rebind_required_ && no_pending;
    primed_ = can_prime;
    previous_thread_ = can_prime ? present_thread_ : ThreadToken{};
    previous_frame_ = can_prime ? present_frame_ : 0;
    present_ticket_ = 0; present_thread_ = {};
    boundary_ambiguous_ = false;
    ClearInterval();
    return published ? WindowResult::Published : !was_primed && can_prime ? WindowResult::Primed : WindowResult::Rejected;
}

std::uint64_t SceneWindow::BeginProbe(ThreadToken thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Eligible(thread)) return 0;
    if (probe_ticket_ || scope_ticket_ || draw_ticket_) { RequireRebind(); return 0; }
    const auto ticket = NextTicket();
    if (!ticket) return 0;
    probe_ticket_ = ticket; probe_thread_ = thread; active_probe_matches_ = false;
    return ticket;
}
bool SceneWindow::EndProbe(std::uint64_t ticket, ThreadToken thread, bool pass_known, std::uint32_t pass,
                           bool stable, const ProbeCamera& camera) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ticket || ticket != probe_ticket_ || !SameThread(thread, probe_thread_) || present_ticket_) {
        RequireRebind(); return false;
    }
    probe_ticket_ = 0; probe_thread_ = {};
    if (!pass_known) { tainted_ = true; return false; }
    if (pass != pass_) return true;
    if (!stable || !ValidCamera(camera) || (camera_ && !SameCamera(*camera_, camera))) {
        tainted_ = true; return false;
    }
    camera_ = camera; active_probe_matches_ = true;
    return true;
}

std::uint64_t SceneWindow::BeginMesh(ThreadToken thread, std::uint32_t pass, std::uint32_t stage,
                                     const MeshSelection& section) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Eligible(thread)) return 0;
    if (probe_ticket_ || scope_ticket_ || draw_ticket_) { RequireRebind(); return 0; }
    if (pass != pass_ || stage != stage_) return 0;
    if (!active_probe_matches_ || section.sections.size() != 1 || section.material_ids.size() > 1024 ||
        section.instance_id.empty() || section.instance_id.size() > 512 ||
        section.resource_id.empty() || section.resource_id.size() > 512 || !section.generation) {
        tainted_ = true; return 0;
    }
    auto copy = section;
    const auto ticket = NextTicket(); if (!ticket) return 0;
    mesh_ = std::move(copy); bsp_.reset(); scope_ticket_ = ticket; scope_thread_ = thread; scope_draws_ = 0;
    return ticket;
}
std::uint64_t SceneWindow::BeginBsp(ThreadToken thread, std::uint32_t pass, std::uint32_t stage,
                                    const BspSelection& section) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Eligible(thread)) return 0;
    if (probe_ticket_ || scope_ticket_ || draw_ticket_) { RequireRebind(); return 0; }
    if (pass != pass_ || stage != stage_) return 0;
    if (!active_probe_matches_ || section.sections.size() != 1 || section.resource_id.empty() ||
        section.resource_id.size() > 512) { tainted_ = true; return 0; }
    auto copy = section;
    const auto ticket = NextTicket(); if (!ticket) return 0;
    bsp_ = std::move(copy); mesh_.reset(); scope_ticket_ = ticket; scope_thread_ = thread; scope_draws_ = 0;
    return ticket;
}
std::uint64_t SceneWindow::BeginDraw(ThreadToken thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Eligible(thread)) return 0;
    if (draw_ticket_ || probe_ticket_ || (scope_ticket_ && !SameThread(thread, scope_thread_))) {
        RequireRebind(); return 0;
    }
    const auto ticket = NextTicket(); if (!ticket) return 0;
    draw_ticket_ = ticket; draw_thread_ = thread;
    draw_scoped_ = scope_ticket_ != 0; draw_scope_ticket_ = scope_ticket_;
    return ticket;
}
bool SceneWindow::EndDraw(std::uint64_t ticket, ThreadToken thread, bool succeeded,
                          bool viewport_stable, const Viewport& viewport) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ticket || ticket != draw_ticket_ || !SameThread(thread, draw_thread_) || present_ticket_ ||
        (draw_scoped_ && scope_ticket_ != draw_scope_ticket_)) {
        RequireRebind(); return false;
    }
    const bool scoped = draw_scoped_;
    draw_ticket_ = 0; draw_thread_ = {}; draw_scoped_ = false; draw_scope_ticket_ = 0;
    if (!succeeded) { tainted_ = true; return false; }
    if (!scoped) return true;
    if (!viewport_stable || !ValidViewport(viewport) ||
        (viewport_ && !SameViewport(*viewport_, viewport)) || scope_draws_ == UINT32_MAX) {
        tainted_ = true; return false;
    }
    viewport_ = viewport; ++scope_draws_;
    return true;
}

bool SceneWindow::MergeScope() {
    if (mesh_) {
        const auto found = mesh_index_.find(mesh_->instance_id);
        if (found == mesh_index_.end()) {
            if (meshes_.size() + blocks_.size() >= 8192) return false;
            const auto index = meshes_.size();
            meshes_.push_back(*mesh_);
            try { mesh_index_.emplace(mesh_->instance_id, index); }
            catch (...) { meshes_.pop_back(); throw; }
            return true;
        }
        auto& item = meshes_[found->second];
        if (item.resource_id != mesh_->resource_id || item.generation != mesh_->generation ||
            item.binding != mesh_->binding ||
            std::memcmp(item.transform.data(), mesh_->transform.data(), sizeof(float)*item.transform.size()) != 0 ||
            item.material_ids != mesh_->material_ids) return false;
        const auto section = mesh_->sections[0];
        if (std::find(item.sections.begin(), item.sections.end(), section) == item.sections.end()) {
            if (item.sections.size() >= 1024) return false;
            item.sections.push_back(section);
        }
        return true;
    }
    if (!bsp_) return false;
    const auto found = block_index_.find(bsp_->resource_id);
    if (found == block_index_.end()) {
        if (meshes_.size() + blocks_.size() >= 8192) return false;
        const auto index = blocks_.size();
        blocks_.push_back(*bsp_);
        try { block_index_.emplace(bsp_->resource_id, index); }
        catch (...) { blocks_.pop_back(); throw; }
        return true;
    }
    auto& item = blocks_[found->second];
    if (item.binding != bsp_->binding) return false;
    const auto section = bsp_->sections[0];
    if (std::find(item.sections.begin(), item.sections.end(), section) == item.sections.end()) {
        if (item.sections.size() >= 1024) return false;
        item.sections.push_back(section);
    }
    return true;
}
bool SceneWindow::EndScope(std::uint64_t ticket, ThreadToken thread, bool stable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ticket || ticket != scope_ticket_ || !SameThread(thread, scope_thread_) || present_ticket_ || draw_ticket_) {
        RequireRebind(); return false;
    }
    const bool ok = stable && (!scope_draws_ || MergeScope());
    scope_ticket_ = 0; scope_thread_ = {}; scope_draws_ = 0; mesh_.reset(); bsp_.reset();
    if (!ok) tainted_ = true;
    return ok;
}
void SceneWindow::MarkLost() {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireRebind();
}
bool SceneWindow::needs_rebind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rebind_required_;
}
} // namespace t3render::live
