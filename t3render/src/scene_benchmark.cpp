#include "scene_benchmark.hpp"
#include "scene_live_lighting.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace t3render::live {
namespace {
constexpr char kMap[] = "SouthQuarter1_int1";
constexpr char kMapHash[] = "ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";

bool Token(std::istream& in, const char* expected) {
    std::string s; return bool(in >> s) && s == expected;
}
bool Id(std::istream& in, std::string& s) {
    if (!(in >> s) || s.empty() || s.size()>512) return false;
    return std::all_of(s.begin(),s.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') ||
               (c>='0' && c<='9') || c=='_' || c==':' || c=='.' || c=='-';
    });
}
template<class T> bool Number(std::istream& in,T& value) {
    std::string s;
    if (!(in >> s) || s.empty() || s.size()>20) return false;
    T result=0;
    for (unsigned char c:s) {
        if (c<'0' || c>'9') return false;
        const T digit=static_cast<T>(c-'0');
        if (result>(std::numeric_limits<T>::max()-digit)/10) return false;
        result=result*10+digit;
    }
    value=result;return true;
}
template<std::size_t N> bool Words(std::istream& in,std::array<std::uint32_t,N>& values) {
    for (auto& v:values) if (!Number(in,v)) return false;
    return true;
}
bool MatrixWords(std::istream& in,Matrix& m) {
    std::array<std::uint32_t,16> words{};
    if (!Words(in,words)) return false;
    std::memcpy(m.data(),words.data(),sizeof(words));return true;
}
bool Sections(std::istream& in,std::vector<std::uint32_t>& sections,std::size_t max) {
    std::uint32_t count=0;
    if (!Number(in,count) || !count || count>max || count>1024) return false;
    std::set<std::uint32_t> unique;
    for (std::uint32_t i=0;i<count;++i) {
        std::uint32_t n=0;
        if (!Number(in,n) || n>=max || !unique.insert(n).second) return false;
        sections.push_back(n);
    }
    return true;
}
bool Link(std::istream& in,scene_bridge::CandleLinkObservation& link) {
    std::uint32_t status=0,enabled=0,inherited=0,transform=0;
    if (!Token(in,"C") || !Number(in,status) || !Number(in,link.factor_8c_bits) ||
        !Number(in,enabled) || !Number(in,inherited) || !Number(in,transform) ||
        status>2 || enabled>2 || inherited>1 || transform>1 || !Words(in,link.transform_bits)) return false;
    link.status=static_cast<scene_bridge::CandleLinkStatus>(status);
    link.enabled.status=static_cast<scene_bridge::CandleEnabledStatus>(enabled);
    link.enabled.inherited=inherited!=0;link.transform_available=transform!=0;
    return ValidCandleLink(link) && link.transform_available;
}
bool Batch(std::istream& in,const char* name,std::shared_ptr<RawLightBatch>& batch) {
    std::uint32_t count=0;
    if (!Token(in,"STATE") || !Token(in,name)) return false;
    batch=std::make_shared<RawLightBatch>();batch->candle_actor_number=14;
    if (!Token(in,"LIGHTS") || !Number(in,count) || count>scene_bridge::kMaxVisibleLightCandidates ||
        !Link(in,batch->candle_link) || !Link(in,batch->other_candle_link)) return false;
    batch->listed=count;
    for (std::uint32_t i=0;i<count;++i) {
        RawLightItem r;
        if (!Token(in,"L") || !Id(in,r.candidate.id) || !Number(in,r.candidate.generation) ||
            !Words(in,r.field_24) || !Words(in,r.field_34) || !Words(in,r.field_68) ||
            !Number(in,r.field_58) || !Number(in,r.field_5c) || !Number(in,r.field_60) ||
            !Number(in,r.field_64) || !Number(in,r.field_74) || !Number(in,r.field_78) ||
            !Number(in,r.field_7c) || !Number(in,r.field_88) || !Number(in,r.field_8c) ||
            !Number(in,r.field_90)) return false;
        r.candidate.binding=BindingEvidence::Candidate;
        Vec3 p{};std::memcpy(p.data(),r.field_34.data(),sizeof(float)*3);
        if (!std::all_of(p.begin(),p.end(),[](float v){return std::isfinite(v) && std::fabs(v)<=1000000;})) return false;
        r.candidate.position={p,BindingEvidence::Candidate};
        batch->items.push_back(std::move(r));
    }
    return true;
}
RawLightFrame MakeRaw(const Snapshot& snapshot,std::shared_ptr<RawLightBatch> batch) {
    RawLightFrame raw;raw.map_epoch=snapshot.map_epoch;raw.device_epoch=snapshot.device_epoch;
    raw.continuity=snapshot.continuity;raw.frame_id=snapshot.frame_id;
    raw.provenance=snapshot.provenance;raw.camera=snapshot.camera;raw.batch=std::move(batch);return raw;
}
} // namespace

bool LoadBenchmarkScene(std::istream& in,LoadedCatalog& catalog,BenchmarkScene& output) {
    if (!catalog.residency || catalog.binding.map!=kMap || catalog.binding.map_sha256!=kMapHash ||
        !catalog.bed_override || !catalog.bedroom_materials || !Token(in,"T3BENCH") || !Token(in,"1")) return false;
    FrameInput f;const auto epoch=catalog.residency->epochs();
    f.map_epoch=epoch.map;f.device_epoch=epoch.device;f.render_pass=0;f.stage=1;
    f.synchronized=true;f.provenance.origin=FrameOrigin::Synthetic;
    f.provenance.session_id="t3bench-frozen";f.conventions=catalog.conventions;
    std::uint32_t min_depth=0,max_depth=0;
    if (!Token(in,"CAMERA") || !MatrixWords(in,f.camera.view) || !MatrixWords(in,f.camera.projection) ||
        !Number(in,f.camera.x) || !Number(in,f.camera.y) || !Number(in,f.camera.width) ||
        !Number(in,f.camera.height) || !Number(in,min_depth) || !Number(in,max_depth) ||
        !f.camera.width || !f.camera.height || f.camera.width>2048 || f.camera.height>2048) return false;
    std::memcpy(&f.camera.min_depth,&min_depth,sizeof(float));std::memcpy(&f.camera.max_depth,&max_depth,sizeof(float));
    f.camera.viewport_source=ViewportSource::ScopedDraw;
    std::uint32_t count=0;
    if (!Token(in,"MESHES") || !Number(in,count) || !count || count>4096) return false;
    for (std::uint32_t i=0;i<count;++i) {
        std::string id;
        if (!Token(in,"M") || !Id(in,id)) return false;
        const auto found=catalog.binding.static_mesh_actors.find(id);
        if (found==catalog.binding.static_mesh_actors.end() || !found->second.resource) return false;
        MeshSelection m;m.instance_id=id;m.resource_id=found->second.resource->id;
        m.matched_resource=found->second.resource;m.material_ids=found->second.material_ids;
        m.binding=BindingEvidence::Candidate;m.identity_scope=IdentityScope::VisibleList;m.generation=1;
        if (!MatrixWords(in,m.transform) || !Sections(in,m.sections,m.matched_resource->geometry.sections.size())) return false;
        f.meshes.push_back(std::move(m));
    }
    if (std::none_of(f.meshes.begin(),f.meshes.end(),[&](const auto& m){return m.instance_id==catalog.binding.bed_instance_id;})) return false;
    if (!Token(in,"BSP") || !Number(in,count) || !count || count>4096) return false;
    for (std::uint32_t i=0;i<count;++i) {
        std::string id;if (!Token(in,"B") || !Id(in,id)) return false;
        auto found=std::find_if(catalog.binding.bsp.begin(),catalog.binding.bsp.end(),[&](const auto& b){return b && b->id==id;});
        if (found==catalog.binding.bsp.end()) return false;
        BspSelection b;b.resource_id=id;b.matched_resource=*found;b.binding=BindingEvidence::Candidate;
        b.identity_scope=IdentityScope::VisibleList;
        if (!Sections(in,b.sections,b.matched_resource->geometry.sections.size())) return false;
        f.bsp.push_back(std::move(b));
    }
    std::shared_ptr<RawLightBatch> on,off;
    if (!Batch(in,"ON",on) || !Batch(in,"OFF",off) || !Token(in,"END")) return false;
    in >> std::ws;if (!in.eof()) return false;
    BenchmarkScene result;f.light_coverage=LightCoverage::Partial;f.frame_id=1;
    for (const auto& item:on->items) f.lights.push_back(item.candidate);
    if (!catalog.residency->Publish(f)) return false;
    result.on=catalog.residency->current();if (!result.on) return false;
    result.on_lights=MakeRaw(*result.on,on);
    f.frame_id=2;f.lights.clear();for (const auto& item:off->items) f.lights.push_back(item.candidate);
    if (!catalog.residency->Publish(f)) return false;
    result.off=catalog.residency->current();if (!result.off) return false;
    result.off_lights=MakeRaw(*result.off,off);
    RenderLight light;
    if (!RawLightFrameMatches(result.on_lights,*result.on) || !RawLightFrameMatches(result.off_lights,*result.off) ||
        RemasterCandleFromRaw(*result.on,result.on_lights,light,14)!=RemasterCandleState::On ||
        RemasterCandleFromRaw(*result.on,result.on_lights,light,6)!=RemasterCandleState::On ||
        RemasterCandleFromRaw(*result.off,result.off_lights,light,14)!=RemasterCandleState::Off ||
        RemasterCandleFromRaw(*result.off,result.off_lights,light,6)!=RemasterCandleState::On) return false;
    output=std::move(result);return true;
}

bool BenchmarkFromLive(const BenchmarkScene& golden,const Snapshot& live,BenchmarkScene& output,
                       std::size_t& matched_meshes,std::size_t& matched_bsp) {
    if (!golden.on || !golden.off || live.map!=kMap || live.map_sha256!=kMapHash ||
        live.provenance.origin!=FrameOrigin::LiveProcess || live.provenance.session_id.empty() ||
        !live.map_epoch || !live.device_epoch || !live.continuity || !live.frame_id || live.render_pass!=0 || live.stage!=1 ||
        !live.camera.width || !live.camera.height ||
        !std::all_of(live.camera.view.begin(),live.camera.view.end(),[](float v){return std::isfinite(v);}) ||
        !std::all_of(live.camera.projection.begin(),live.camera.projection.end(),[](float v){return std::isfinite(v);}) ||
        !RawLightFrameMatches(golden.on_lights,*golden.on) || !RawLightFrameMatches(golden.off_lights,*golden.off)) return false;
    const auto valid_evidence=[](BindingEvidence binding,IdentityScope scope) {
        return (binding==BindingEvidence::Candidate || binding==BindingEvidence::Verified) &&
               (scope==IdentityScope::VisibleList || scope==IdentityScope::FullObjectScan);
    };
    auto on=std::make_shared<Snapshot>(*golden.on);auto off=std::make_shared<Snapshot>(*golden.off);
    std::size_t meshes=0,bsp=0;
    for (auto& target:on->meshes) {
        const auto found=std::find_if(live.meshes.begin(),live.meshes.end(),[&](const auto& candidate) {
            return candidate.selection.instance_id==target.selection.instance_id;
        });
        if (found==live.meshes.end()) continue;
        if (!found->resource || !target.resource || found->resource->id!=target.resource->id ||
            found->selection.resource_id!=target.selection.resource_id ||
            !found->selection.generation || found->selection.sections.empty() ||
            !valid_evidence(found->selection.binding,found->selection.identity_scope) ||
            !std::all_of(found->selection.transform.begin(),found->selection.transform.end(),[](float v){return std::isfinite(v);})) return false;
        target.selection.transform=found->selection.transform;
        auto other=std::find_if(off->meshes.begin(),off->meshes.end(),[&](const auto& candidate) {
            return candidate.selection.instance_id==target.selection.instance_id;
        });
        if (other==off->meshes.end()) return false;
        other->selection.transform=found->selection.transform;++meshes;
    }
    for (const auto& target:on->bsp) {
        const auto found=std::find_if(live.bsp.begin(),live.bsp.end(),[&](const auto& candidate) {
            return candidate.selection.resource_id==target.selection.resource_id;
        });
        if (found==live.bsp.end()) continue;
        if (!found->resource || !target.resource || found->resource->id!=target.resource->id ||
            found->selection.sections.empty() || !valid_evidence(found->selection.binding,found->selection.identity_scope)) return false;
        ++bsp;
    }
    if (!meshes && !bsp) return false;
    BenchmarkScene result=golden;result.on=std::move(on);result.off=std::move(off);
    output=std::move(result);matched_meshes=meshes;matched_bsp=bsp;return true;
}
} // namespace t3render::live
