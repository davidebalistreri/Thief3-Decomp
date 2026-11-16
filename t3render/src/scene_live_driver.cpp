#include "scene_live_driver.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace t3render::live {
namespace {
constexpr std::size_t kMaxPending=1024;
bool SameThread(ThreadToken a, ThreadToken b) {
    return a.id && a.birth && a.id==b.id && a.birth==b.birth;
}
bool SameViewport(const Viewport& a, const Viewport& b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height &&
        std::memcmp(&a.min_depth,&b.min_depth,sizeof(float))==0 &&
        std::memcmp(&a.max_depth,&b.max_depth,sizeof(float))==0;
}
struct Identity {
    std::uint32_t owner=0,index=0,fname=0,node=0,proxy=0,resource=0;
};
Identity IdentityOf(const scene_bridge::SubmissionSample& sample) {
    return {sample.owner,sample.owner_index,sample.owner_fname,sample.mesh_node,sample.asset_proxy,sample.asset_resource};
}
bool SameIdentity(const Identity& a,const Identity& b) {
    return a.owner==b.owner && a.index==b.index && a.fname==b.fname && a.node==b.node &&
        a.proxy==b.proxy && a.resource==b.resource;
}
} // namespace

struct SceneDriver::Impl {
    struct Probe { ThreadToken thread; std::uint64_t window=0; };
    struct LightSample { ThreadToken thread; std::uint64_t window=0; };
    struct Scope {
        ThreadToken thread;std::uint64_t window=0;scene_bridge::SubmissionSample before;
        std::shared_ptr<const Resource> resource;ProbeCamera camera;std::uint32_t stage=0,section=0;
        bool sampled=false;
    };
    struct Draw {
        ThreadToken thread;std::uint64_t window=0,scope=0;IndexedDraw arguments;
        std::shared_ptr<const Resource> resource;ProbeCamera camera;scene_bridge::SubmissionSample before;
        std::uint32_t section=0;
    };
    struct Present { ThreadToken thread;std::uint64_t window=0; };
    struct Reset { ThreadToken thread;std::uint64_t residency=0; };
    struct CachedBsp { scene_bridge::SubmissionSample sample;BspSelection selection; };
    struct ActorTrack { Identity identity{};std::uint64_t generation=0,last_seen=0;bool known=false; };
    mutable std::mutex mutex;
    LoadedCatalog catalog;
    FrameProvenance provenance;
    const std::uint32_t pass,stage;
    std::unique_ptr<SceneWindow> window;
    std::unordered_map<std::uint64_t,Probe> probes;
    std::unordered_map<std::uint64_t,LightSample> light_samples;
    std::unordered_map<std::uint64_t,Scope> scopes;
    std::unordered_map<std::uint64_t,Draw> draws;
    std::unordered_map<std::uint64_t,Present> presents;
    std::unordered_map<std::uint64_t,Reset> resets;
    std::unordered_map<std::uint32_t,CachedBsp> bsp_matches;
    std::unordered_map<std::string,ActorTrack> actor_tracks;
    std::shared_ptr<const scene_bridge::ProbeSnapshot> latest;
    ProbeCamera latest_camera;
    ThreadToken latest_thread{};
    std::uint64_t next_ticket=1,next_frame=0,generation=0,last_seen_frame=0;
    Identity identity{};
    DriverStats diagnostics;
    bool identity_known=false;
    std::atomic<bool> disabled{false};

    Impl(LoadedCatalog input,FrameProvenance source,std::uint32_t selected_pass,std::uint32_t selected_stage)
        : catalog(std::move(input)),provenance(std::move(source)),pass(selected_pass),stage(selected_stage) {}
    std::uint64_t Ticket() {
        if (!next_ticket || next_ticket==UINT64_MAX) { disabled.store(true);return 0; }
        return next_ticket++;
    }
    bool Empty() const { return probes.empty() && light_samples.empty() && scopes.empty() && draws.empty() && presents.empty() && resets.empty(); }
    void Lost() {
        ++diagnostics.losses;
        latest.reset();latest_thread={};bsp_matches.clear();
        for (auto& item:actor_tracks) item.second.known=false;
        if (window) window->MarkLost();
    }
    void Disable() noexcept {
        disabled.store(true);
        try {
            const auto epoch=catalog.residency->epochs();
            (void)catalog.residency->DiscardFrame(epoch.map,epoch.device,UINT64_MAX);
        } catch (...) {}
    }
    bool Rebind() {
        if (!Empty() || !catalog.residency->epochs().ready) return false;
        const auto epoch=catalog.residency->epochs();
        window=std::make_unique<SceneWindow>(*catalog.residency,epoch,provenance,catalog.conventions,pass,stage);
        latest.reset();latest_thread={};bsp_matches.clear();identity_known=false;
        for (auto& item:actor_tracks) item.second.known=false;
        return true;
    }
};

SceneDriver::SceneDriver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SceneDriver::~SceneDriver() = default;
std::unique_ptr<SceneDriver> SceneDriver::Create(LoadedCatalog catalog,FrameProvenance provenance,
                                                 std::uint32_t pass,std::uint32_t stage) noexcept {
    try {
        if (!catalog.residency || !catalog.binding.bed || catalog.binding.bsp.empty() ||
            !catalog.binding.bsp_catalog_complete || provenance.origin==FrameOrigin::Unknown || provenance.session_id.empty()) return {};
        const auto epoch=catalog.residency->epochs();
        if (!epoch.map || !epoch.ready || catalog.residency->FindSceneResource(epoch.map,catalog.binding.bed->id)!=catalog.binding.bed) return {};
        for (const auto& bsp:catalog.binding.bsp)
            if (!bsp || catalog.residency->FindSceneResource(epoch.map,bsp->id)!=bsp) return {};
        if (catalog.binding.static_mesh_actors.size()>8192) return {};
        for (const auto& [id,candidate]:catalog.binding.static_mesh_actors) {
            if (candidate.instance_id!=id || !candidate.resource ||
                catalog.residency->FindSceneResource(epoch.map,candidate.resource->id)!=candidate.resource ||
                candidate.material_ids.size()!=candidate.resource->geometry.sections.size()) return {};
            for (const auto& material:candidate.material_ids)
                if (!material || !catalog.binding.materials.count(*material)) return {};
        }
        auto impl=std::make_unique<Impl>(std::move(catalog),std::move(provenance),pass,stage);
        if (!impl->Rebind()) return {};
        return std::unique_ptr<SceneDriver>(new SceneDriver(std::move(impl)));
    } catch (...) { return {}; }
}

std::uint64_t SceneDriver::BeginProbe(ThreadToken thread) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return 0;
        if (s.probes.size()>=kMaxPending) { s.Disable();return 0; }
        if (!s.probes.empty() || !s.light_samples.empty() || !s.scopes.empty() || !s.draws.empty() || !s.presents.empty() || !s.resets.empty()) s.Lost();
        const auto id=s.Ticket();if (!id) { s.Disable();return 0; }
        const auto ticket=s.window ? s.window->BeginProbe(thread) : 0;
        s.latest.reset();s.latest_thread={};s.bsp_matches.clear();s.probes.emplace(id,Impl::Probe{thread,ticket});return id;
    } catch (...) { impl_->Disable();return 0; }
}
void SceneDriver::EndProbe(std::uint64_t ticket,ThreadToken thread,const scene_bridge::ProbeSnapshot& snapshot) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return;
        const auto item=s.probes.find(ticket);
        if (!ticket || item==s.probes.end()) { s.Lost();return; }
        const auto record=item->second;s.probes.erase(item);
        ++s.diagnostics.probes;
        if (!SameThread(thread,record.thread) || snapshot.thread_id!=thread.id) { s.Lost();return; }
        if (!record.window || !s.window) return;
        ProbeCamera camera;
        const bool selected=snapshot.pass.available && snapshot.pass.value==s.pass;
        const bool stable=selected && ReadProbeCamera(snapshot,thread.id,s.pass,camera);
        const bool accepted=s.window->EndProbe(record.window,thread,snapshot.pass.available,snapshot.pass.value,stable,camera);
        if (selected && stable && ClassifyMapWitness(s.catalog.binding,snapshot)==MapWitnessStatus::Conflicts) {
            if (accepted) ++s.diagnostics.selected_probes;
            ++s.diagnostics.map_witness_conflicts;
            // Discard both the previous snapshot and any partial selection
            // made earlier in this Present interval. This witness is not a
            // map-change notification, so do not invent a new map epoch.
            s.Lost();return;
        }
        if (accepted && selected && stable) {
            ++s.diagnostics.selected_probes;
            s.latest=std::make_shared<const scene_bridge::ProbeSnapshot>(snapshot);s.latest_camera=camera;s.latest_thread=thread;
        } else { if (selected) ++s.diagnostics.selected_probe_bad;s.latest.reset();s.latest_thread={}; }
    } catch (...) { impl_->Disable(); }
}

std::uint64_t SceneDriver::BeginLightSample(ThreadToken thread) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load() || !s.window || !s.latest) return 0;
        if (s.light_samples.size()>=kMaxPending) {s.Disable();return 0;}
        if (!SameThread(thread,s.latest_thread) || !s.Empty()) {s.Lost();return 0;}
        const auto id=s.Ticket();if (!id) {s.Disable();return 0;}
        const auto window=s.window->BeginLightSample(thread);
        s.light_samples.emplace(id,Impl::LightSample{thread,window});return id;
    } catch (...) {impl_->Disable();return 0;}
}
bool SceneDriver::EndLightSample(std::uint64_t ticket,ThreadToken thread,bool stable,LightCoverage coverage,
                                 const std::vector<VisualLight>& lights) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return false;
        const auto item=s.light_samples.find(ticket);
        if (!ticket || item==s.light_samples.end()) {s.Lost();return false;}
        const auto record=item->second;s.light_samples.erase(item);
        if (!SameThread(thread,record.thread)) {s.Lost();return false;}
        return record.window && s.window && s.window->EndLightSample(record.window,thread,stable,coverage,lights);
    } catch (...) {impl_->Disable();return false;}
}

std::uint64_t SceneDriver::BeginScope(ThreadToken thread,std::uint32_t stage,std::uint32_t section) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return 0;
        if (s.scopes.size()>=kMaxPending) { s.Disable();return 0; }
        if (!s.scopes.empty() || !s.probes.empty() || !s.light_samples.empty() || !s.draws.empty() || !s.presents.empty() || !s.resets.empty()) s.Lost();
        const auto id=s.Ticket();if (!id) { s.Disable();return 0; }
        Impl::Scope scope;scope.thread=thread;scope.stage=stage;scope.section=section;
        s.scopes.emplace(id,std::move(scope));return id;
    } catch (...) { impl_->Disable();return 0; }
}
std::shared_ptr<const scene_bridge::ProbeSnapshot> SceneDriver::GenericProbe(
        std::uint64_t ticket,ThreadToken thread,const scene_bridge::SubmissionSample& before) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        const auto scope=s.scopes.find(ticket);
        if (s.disabled.load() || !ticket || scope==s.scopes.end() || scope->second.sampled ||
            !SameThread(thread,scope->second.thread) || !s.latest || !SameThread(thread,s.latest_thread) ||
            scope->second.stage!=s.stage || !before.valid || before.pass!=s.pass || before.type!=4 ||
            before.vtable!=0x10e4e8c0 || !(before.owner_fname>>16)) return {};
        const auto id="actor:"+s.catalog.binding.map+":"+s.catalog.binding.map_sha256+
                      ":StaticMeshActor__"+std::to_string((before.owner_fname>>16)-1);
        if (id==s.catalog.binding.bed_instance_id || !s.catalog.binding.static_mesh_actors.count(id)) return {};
        return s.latest;
    } catch (...) {impl_->Disable();return {};}
}
bool SceneDriver::SetScopeSample(std::uint64_t ticket,ThreadToken thread,
                                 const scene_bridge::SubmissionSample& before,
                                 const scene_bridge::ProbeStaticMesh* generic) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return false;
        const auto item=s.scopes.find(ticket);
        if (!ticket || item==s.scopes.end() || !SameThread(thread,item->second.thread) || item->second.sampled) {
            s.Lost();return false;
        }
        auto& scope=item->second;scope.sampled=true;scope.before=before;
        if (!s.presents.empty() || !s.resets.empty() || !s.probes.empty() || !s.light_samples.empty() || !s.draws.empty() || s.scopes.size()!=1) {
            s.Lost();return false;
        }
        if (s.window && s.latest && SameThread(thread,s.latest_thread) && scope.stage==s.stage && before.pass==s.pass) {
            const auto cached=s.bsp_matches.find(before.element);
            if (cached!=s.bsp_matches.end() && !scene_bridge::SameSubmission(cached->second.sample,before)) {
                s.Lost();return false;
            }
            MeshSelection mesh;BspSelection bsp;
            auto next_generation=s.generation;
            const auto identity=IdentityOf(before);
            const bool new_identity=!s.identity_known || !SameIdentity(identity,s.identity) ||
                (s.last_seen_frame<s.next_frame && s.next_frame-s.last_seen_frame>1);
            if (new_identity) {
                if (next_generation==UINT64_MAX) { s.Disable();return false; }
                ++next_generation;
            }
            if (ResolveBed(s.catalog.binding,*s.latest,before,scope.section,next_generation,mesh)) {
                scope.window=s.window->BeginMesh(thread,s.pass,scope.stage,mesh);
                if (scope.window) {
                    ++s.diagnostics.bed_scopes;
                    scope.resource=mesh.matched_resource;scope.camera=s.latest_camera;
                    s.identity=identity;s.identity_known=true;s.generation=next_generation;s.last_seen_frame=s.next_frame;
                }
            } else {
                bool matched=false;
                if (generic) ++s.diagnostics.static_mesh_samples;
                if (generic && (before.owner_fname>>16)) {
                    const auto id="actor:"+s.catalog.binding.map+":"+s.catalog.binding.map_sha256+
                                  ":StaticMeshActor__"+std::to_string((before.owner_fname>>16)-1);
                    if (s.catalog.binding.static_mesh_actors.count(id) && id!=s.catalog.binding.bed_instance_id) {
                        auto& track=s.actor_tracks[id];auto generation=track.generation;
                        const bool changed=!track.known || !SameIdentity(track.identity,identity) ||
                            (track.last_seen<s.next_frame && s.next_frame-track.last_seen>1);
                        if (changed) {if (generation==UINT64_MAX) {s.Disable();return false;}++generation;}
                        MeshSelection selected;
                        if (ResolveStaticMesh(s.catalog.binding,*s.latest,before,*generic,scope.section,generation,selected)) {
                            scope.window=s.window->BeginMesh(thread,s.pass,scope.stage,selected);
                            if (scope.window) {
                                matched=true;++s.diagnostics.static_mesh_scopes;
                                scope.resource=selected.matched_resource;scope.camera=s.latest_camera;
                                track.identity=identity;track.generation=generation;track.last_seen=s.next_frame;track.known=true;
                            }
                        }
                    }
                }
                if (generic && !matched) ++s.diagnostics.static_mesh_rejected;
                if (matched) return true;
                if (cached!=s.bsp_matches.end()) {
                    if (cached->second.selection.matched_resource &&
                        scope.section<cached->second.selection.matched_resource->geometry.sections.size()) {
                        bsp=cached->second.selection;bsp.sections={scope.section};matched=true;++s.diagnostics.bsp_cache_hits;
                    }
                } else if (ResolveBsp(s.catalog.binding,*s.latest,before,scope.section,bsp)) matched=true;
                if (matched) {
                    scope.window=s.window->BeginBsp(thread,s.pass,scope.stage,bsp);
                    if (scope.window) {
                        if (cached==s.bsp_matches.end()) s.bsp_matches.emplace(before.element,Impl::CachedBsp{before,bsp});
                        ++s.diagnostics.bsp_scopes;scope.resource=bsp.matched_resource;scope.camera=s.latest_camera;
                    }
                }
            }
        }
        return true;
    } catch (...) { impl_->Disable();return false; }
}
void SceneDriver::EndScope(std::uint64_t ticket,ThreadToken thread,const scene_bridge::SubmissionSample& after) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return;
        const auto item=s.scopes.find(ticket);
        if (!ticket || item==s.scopes.end()) { s.Lost();return; }
        auto record=std::move(item->second);s.scopes.erase(item);
        if (!SameThread(thread,record.thread) || !record.sampled) { s.Lost();return; }
        for (const auto& draw:s.draws) if (draw.second.scope==ticket) { s.Lost();break; }
        if (record.window && s.window) {
            const bool unchanged=ScopeUnchanged(record.before,after);
            if (!unchanged) ++s.diagnostics.scope_unstable;
            if (!s.window->EndScope(record.window,thread,unchanged)) ++s.diagnostics.scope_rejected;
        }
    } catch (...) { impl_->Disable(); }
}

DrawStart SceneDriver::BeginDraw(ThreadToken thread,std::uint64_t enclosing_scope,const IndexedDraw& draw) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return {};
        if (s.draws.size()>=kMaxPending) { s.Disable();return {}; }
        if (!s.draws.empty() || !s.probes.empty() || !s.light_samples.empty() || !s.presents.empty() || !s.resets.empty()) s.Lost();
        const auto id=s.Ticket();if (!id) { s.Disable();return {}; }
        Impl::Draw record;record.thread=thread;record.scope=enclosing_scope;record.arguments=draw;
        if (enclosing_scope) {
            const auto scope=s.scopes.find(enclosing_scope);
            if (scope==s.scopes.end() || !SameThread(thread,scope->second.thread)) s.Lost();
            else if (scope->second.window) {
                record.resource=scope->second.resource;record.camera=scope->second.camera;
                record.before=scope->second.before;record.section=scope->second.section;
            }
        } else if (!s.scopes.empty()) s.Lost();
        record.window=s.window ? s.window->BeginDraw(thread) : 0;
        const bool needs=record.window && record.resource;
        s.draws.emplace(id,std::move(record));return {id,needs};
    } catch (...) { impl_->Disable();return {}; }
}
void SceneDriver::EndDraw(std::uint64_t ticket,ThreadToken thread,bool succeeded,
                          const DrawObservation& before,const DrawObservation& after) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return;
        const auto item=s.draws.find(ticket);
        if (!ticket || item==s.draws.end()) { s.Lost();return; }
        auto record=std::move(item->second);s.draws.erase(item);
        if (!SameThread(thread,record.thread)) { s.Lost();return; }
        if (!record.window || !s.window) return;
        const bool geometry=record.resource && ScopeDrawMatches(*record.resource,record.section,record.arguments);
        const bool camera=record.resource && before.available && after.available &&
            ScopeCameraMatches(record.camera,before.view,before.projection) && ScopeCameraMatches(record.camera,after.view,after.projection);
        const bool world=record.resource && before.available && after.available &&
            ScopeWorldMatches(record.before,before.world) && ScopeWorldMatches(record.before,after.world);
        const bool viewport=before.available && after.available && SameViewport(before.viewport,after.viewport);
        if (record.resource) {
            if (!geometry) ++s.diagnostics.draw_geometry_bad;
            if (!camera) ++s.diagnostics.draw_camera_bad;
            if (!world) ++s.diagnostics.draw_world_bad;
            if (!viewport) ++s.diagnostics.draw_viewport_bad;
        }
        (void)s.window->EndDraw(record.window,thread,succeeded,geometry,camera,world,viewport,before.viewport);
    } catch (...) { impl_->Disable(); }
}

std::uint64_t SceneDriver::BeginPresent(ThreadToken thread,std::uint64_t* frame_id) noexcept {
    if (frame_id) *frame_id=UINT64_MAX;
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return 0;
        if (s.presents.size()>=kMaxPending || s.next_frame==UINT64_MAX) { s.Disable();return 0; }
        const bool quiescent=s.Empty();
        if (quiescent && (!s.window || s.window->needs_rebind())) (void)s.Rebind();
        if (!quiescent) s.Lost();
        const auto id=s.Ticket();if (!id) { s.Disable();return 0; }
        const auto frame=s.next_frame++;
        const auto ticket=s.window ? s.window->BeginPresent(thread,frame,quiescent) : 0;
        s.presents.emplace(id,Impl::Present{thread,ticket});
        if (ticket && frame_id) *frame_id=frame;
        return id;
    } catch (...) { impl_->Disable();return 0; }
}
WindowResult SceneDriver::EndPresent(std::uint64_t ticket,ThreadToken thread,bool succeeded) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return WindowResult::Rejected;
        const auto item=s.presents.find(ticket);
        if (!ticket || item==s.presents.end()) { s.Lost();return WindowResult::Rejected; }
        const auto record=item->second;s.presents.erase(item);
        s.latest.reset();s.latest_thread={};
        if (!SameThread(thread,record.thread)) { s.Lost();return WindowResult::Rejected; }
        const auto result=record.window && s.window ? s.window->EndPresent(record.window,thread,succeeded) : WindowResult::Rejected;
        if (result==WindowResult::Published) ++s.diagnostics.published;
        else if (result==WindowResult::Primed) ++s.diagnostics.primed;
        else {
            ++s.diagnostics.rejected;
            // A rejected interval may hide an identity change even when the
            // next visible sample reuses the same address and frame sequence.
            s.identity_known=false;
            for (auto& actor:s.actor_tracks) actor.second.known=false;
        }
        s.diagnostics.last_rejection=record.window && s.window ? s.window->last_rejection() : WindowNeedsRebind;
        return result;
    } catch (...) { impl_->Disable();return WindowResult::Rejected; }
}
std::uint64_t SceneDriver::BeginReset(ThreadToken thread) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return 0;
        if (!s.resets.empty()) { s.Disable();return 0; }
        s.Lost();const auto id=s.Ticket();if (!id) { s.Disable();return 0; }
        const auto ticket=s.catalog.residency->BeginReset();
        if (!ticket) { s.Disable();return 0; }
        s.resets.emplace(id,Impl::Reset{thread,ticket});return id;
    } catch (...) { impl_->Disable();return 0; }
}
void SceneDriver::EndReset(std::uint64_t ticket,ThreadToken thread,bool succeeded) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.load()) return;
        const auto item=s.resets.find(ticket);
        if (!ticket || item==s.resets.end()) { s.Lost();return; }
        const auto record=item->second;s.resets.erase(item);
        if (!SameThread(thread,record.thread) || !record.residency ||
            !s.catalog.residency->EndReset(record.residency,succeeded)) { s.Lost();return; }
        s.identity_known=false;
    } catch (...) { impl_->Disable(); }
}
void SceneDriver::MarkLost() noexcept {
    try { auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);if (!s.disabled.load()) s.Lost(); }
    catch (...) { impl_->Disable(); }
}
std::shared_ptr<const Snapshot> SceneDriver::current() const noexcept {
    try { return impl_->disabled.load() ? nullptr : impl_->catalog.residency->current(); }
    catch (...) { return {}; }
}
DriverStats SceneDriver::stats() const noexcept {
    try { auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);return s.diagnostics; }
    catch (...) { return {}; }
}
bool SceneDriver::SubmitIfCurrent(const std::shared_ptr<const Snapshot>& expected,
                                  bool (*submit)(void*) noexcept, void* context) noexcept {
    try {
        auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
        return !s.disabled.load() && s.catalog.residency->SubmitIfCurrent(expected,submit,context);
    } catch (...) { impl_->Disable();return false; }
}
} // namespace t3render::live
