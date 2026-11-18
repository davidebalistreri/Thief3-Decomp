#include "scene_live_lighting.hpp"
#include "scene_live_candle_flame_geometry.hpp"
#include "scene_live_candlestick_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace t3render::live {
namespace {
bool Digest(const std::string& value) {
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](unsigned char c){
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    });
}
bool ValidProvenance(const FrameProvenance& value) {
    if (value.session_id.empty() || value.session_id.size()>512 || value.session_id.find('\0')!=std::string::npos) return false;
    if (value.session_id.size()>2 && value.session_id[0]=='0' && (value.session_id[1]=='x' || value.session_id[1]=='X') &&
        std::all_of(value.session_id.begin()+2,value.session_id.end(),[](unsigned char c){
            return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');
        })) return false;
    if (value.origin==FrameOrigin::LiveProcess || value.origin==FrameOrigin::Synthetic) return !value.capture_sha256;
    return value.origin==FrameOrigin::CapturedReplay && value.capture_sha256 && Digest(*value.capture_sha256);
}
bool CameraPosition(const Matrix& m,Vec3& output) {
    if (!std::all_of(m.begin(),m.end(),[](float v){return std::isfinite(v);}) ||
        std::fabs(m[3])>1e-6f || std::fabs(m[7])>1e-6f || std::fabs(m[11])>1e-6f ||
        std::fabs(m[15]-1)>1e-6f) return false;
    const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
    const std::array<double,9> cof{e*i-f*h,f*g-d*i,d*h-e*g,
                                   c*h-b*i,a*i-c*g,b*g-a*h,
                                   b*f-c*e,c*d-a*f,a*e-b*d};
    const double determinant=a*cof[0]+b*cof[1]+c*cof[2];
    if (!std::isfinite(determinant) || std::fabs(determinant)<1e-12) return false;
    Vec3 result{};
    for (unsigned col=0;col<3;++col) {
        double value=0;
        for (unsigned row=0;row<3;++row) value-=double(m[12+row])*cof[col*3+row]/determinant;
        if (!std::isfinite(value) || std::fabs(value)>1000000) return false;
        result[col]=static_cast<float>(value);
    }
    output=result;return true;
}
} // namespace

bool CameraPositionFromView(const Matrix& view,Vec3& output) noexcept {
    return CameraPosition(view,output);
}

bool CandlePreviewFromRaw(const Snapshot& snapshot,const RawLightFrame& raw,RenderLight& output) noexcept {
    try {
        constexpr char map[]="SouthQuarter1_int1";
        constexpr char hash[]="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
        if (snapshot.map!=map || snapshot.map_sha256!=hash || !RawLightFrameMatches(raw,snapshot)) return false;
        const std::string id=std::string("light-actor:")+map+":"+hash+":D_384:6";
        const RawLightItem* found=nullptr;
        for (const auto& item:raw.batch->items) if (item.candidate.id==id) {
            if (found) return false;
            found=&item;
        }
        if (!found || !found->candidate.position.value || found->candidate.position.evidence!=BindingEvidence::Candidate)
            return false;
        // The actor-link sample is taken after the partial light list. If they
        // disagree, neither light value is a reliable current-frame preview.
        const auto& link=raw.batch->candle_link;
        if (link.status==scene_bridge::CandleLinkStatus::NoLink ||
            (link.status==scene_bridge::CandleLinkStatus::Candidate && link.factor_8c_bits!=found->field_8c))
            return false;
        Vec3 color{};float maximum=0;
        for (unsigned i=0;i<3;++i) {
            std::memcpy(&color[i],&found->field_68[i],sizeof(float));
            if (!std::isfinite(color[i]) || color[i]<0 || color[i]>1) return false;
            maximum=std::max(maximum,color[i]);
        }
        if (maximum<=0) return false;
        for (auto& value:color) value/=maximum;
        float damping_factor=0;
        std::memcpy(&damping_factor,&found->field_8c,sizeof(float));
        if (!std::isfinite(damping_factor) || damping_factor<0 || damping_factor>1) return false;
        RenderLight result;result.id=id;result.generation=found->candidate.generation;
        result.position=*found->candidate.position.value;result.color_linear=color;
        // 1 cd is artistic. The sampled factor is a candidate modulation,
        // not independent proof of the engine's enabled flag or physical units.
        result.intensity=damping_factor;result.influence_radius_m=3;result.source_radius_m=0.12f;
        output=std::move(result);return true;
    } catch (...) {return false;}
}

bool CandlePreviewDirectZero(const Snapshot& snapshot,const RawLightFrame& raw) noexcept {
    try {
        constexpr char map[]="SouthQuarter1_int1";
        constexpr char hash[]="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
        if (snapshot.map!=map || snapshot.map_sha256!=hash || !RawLightFrameMatches(raw,snapshot) ||
            raw.batch->candle_link.status!=scene_bridge::CandleLinkStatus::Candidate) return false;
        const std::string id=std::string("light-actor:")+map+":"+hash+":D_384:6";
        // If both samplers saw the light, do not override an inconsistent or
        // malformed list candidate with a later actor-only observation.
        for (const auto& item:raw.batch->items) if (item.candidate.id==id) return false;
        float factor=0;std::memcpy(&factor,&raw.batch->candle_link.factor_8c_bits,sizeof(factor));
        return std::isfinite(factor) && factor==0;
    } catch (...) {return false;}
}

namespace {
bool CandleActorTransform(const Snapshot& snapshot,const RawLightFrame& raw,Matrix& output) {
        constexpr char map[]="SouthQuarter1_int1";
        constexpr char hash[]="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
        if (snapshot.map!=map || snapshot.map_sha256!=hash || !RawLightFrameMatches(raw,snapshot) ||
            !snapshot.conventions.meters_per_unit || !std::isfinite(*snapshot.conventions.meters_per_unit) ||
            std::fabs(*snapshot.conventions.meters_per_unit-0.01905f)>1e-8f ||
            snapshot.conventions.length_unit!=LengthUnit::T3Unit || snapshot.conventions.axes!=Axes::T3Xyz ||
            snapshot.conventions.up_axis!=UpAxis::Z ||
            (raw.batch->candle_link.status!=scene_bridge::CandleLinkStatus::Candidate &&
             raw.batch->candle_link.status!=scene_bridge::CandleLinkStatus::NoLink) ||
            !raw.batch->candle_link.transform_available) return false;
        Matrix transform{};
        std::memcpy(transform.data(),raw.batch->candle_link.transform_bits.data(),sizeof(float)*transform.size());
        if (!std::all_of(transform.begin(),transform.end(),[](float v){return std::isfinite(v) && std::fabs(v)<=1000000;}) ||
            std::fabs(transform[3])>1e-6f || std::fabs(transform[7])>1e-6f ||
            std::fabs(transform[11])>1e-6f || std::fabs(transform[15]-1)>1e-6f) return false;
        for (unsigned row=0;row<3;++row) for (unsigned column=0;column<3;++column)
            if (std::fabs(transform[row*4+column])>16) return false;
        for (unsigned axis=0;axis<3;++axis) if (std::fabs(transform[12+axis])>998000) return false;
        const double det=double(transform[0])*(double(transform[5])*transform[10]-double(transform[6])*transform[9])-
            double(transform[1])*(double(transform[4])*transform[10]-double(transform[6])*transform[8])+
            double(transform[2])*(double(transform[4])*transform[9]-double(transform[5])*transform[8]);
        if (!std::isfinite(det) || std::fabs(det)<1e-10) return false;
        output=transform;return true;
}
} // namespace

bool CandlePreviewCandlestick(const Snapshot& snapshot,const RawLightFrame& raw,
                              std::array<ResidentFlatDraw,2>& output) noexcept {
    try {
        Matrix transform{};if (!CandleActorTransform(snapshot,raw,transform)) return false;
        const auto mesh=[]() -> std::shared_ptr<const Resource> {
            static const auto resource=[]() {
                using namespace candlestick_geometry;
                auto result=std::make_shared<Resource>();result->id="preview:generated-candlestick";
                result->name="Candlestick";result->kind=ResourceKind::StaticMesh;result->source_sha256=source_sha256;
                auto append=[&](const auto& positions,const auto& normals,const auto& indices,const char* name) {
                    const auto base=static_cast<std::uint32_t>(result->geometry.positions.size());
                    const auto first=static_cast<std::uint32_t>(result->geometry.indices.size());
                    for (const auto& p:positions) result->geometry.positions.push_back({p[0]/0.01905f,p[2]/0.01905f,p[1]/0.01905f});
                    for (const auto& n:normals) result->geometry.normals.push_back({n[0],n[2],n[1]});
                    for (std::size_t i=0;i<std::size(indices);i+=3) {
                        result->geometry.indices.push_back(base+indices[i]);result->geometry.indices.push_back(base+indices[i+2]);
                        result->geometry.indices.push_back(base+indices[i+1]);
                    }
                    result->geometry.sections.push_back({name,first,static_cast<std::uint32_t>(std::size(indices)),std::nullopt});
                };
                append(metal_positions_m,metal_normals,metal_indices,"metal");
                append(wax_positions_m,wax_normals,wax_indices,"wax");
                return std::shared_ptr<const Resource>(std::move(result));
            }();
            return resource;
        }();
        std::array<ResidentFlatDraw,2> draws{};
        for (std::uint32_t i=0;i<draws.size();++i) {
            draws[i].resource=mesh;draws[i].section=i;draws[i].first_index=mesh->geometry.sections[i].first_index;
            draws[i].index_count=mesh->geometry.sections[i].index_count;draws[i].transform=transform;
        }
        draws[0].color={0.35f,0.31f,0.28f,1};draws[1].color={0.85f,0.75f,0.55f,1};
        output=std::move(draws);return true;
    } catch (...) {return false;}
}

bool CandlePreviewFlame(const Snapshot& snapshot,const RawLightFrame& raw,ResidentFlatDraw& output) noexcept {
    try {
        RenderLight light;Matrix transform{};
        if (!raw.batch || raw.batch->candle_link.status!=scene_bridge::CandleLinkStatus::Candidate ||
            !CandlePreviewFromRaw(snapshot,raw,light) || light.intensity<=0 ||
            !CandleActorTransform(snapshot,raw,transform)) return false;
        const RawLightItem* listed=nullptr;
        for (const auto& item:raw.batch->items) if (item.candidate.id==light.id) {
            if (listed) return false;
            listed=&item;
        }
        if (!listed || listed->field_8c!=raw.batch->candle_link.factor_8c_bits) return false;
        double distance2=0;
        for (unsigned i=0;i<3;++i) {
            const double delta=(double(light.position[i])-transform[12+i])*0.01905;
            distance2+=delta*delta;
        }
        if (!std::isfinite(distance2) || distance2>4) return false;
        const auto mesh=[]() -> std::shared_ptr<const Resource> {
            static const auto resource=[]() {
                using namespace candle_flame_geometry;
                auto result=std::make_shared<Resource>();result->id="preview:generated-candle-flame";
                result->name="CandleFlame";result->kind=ResourceKind::StaticMesh;
                result->source_sha256=source_sha256;
                for (const auto& p:positions_m) result->geometry.positions.push_back({p[0]/0.01905f,p[2]/0.01905f,p[1]/0.01905f});
                for (const auto& n:normals) result->geometry.normals.push_back({n[0],n[2],n[1]});
                for (std::size_t i=0;i<std::size(indices);i+=3) {
                    result->geometry.indices.push_back(indices[i]);result->geometry.indices.push_back(indices[i+2]);
                    result->geometry.indices.push_back(indices[i+1]);
                }
                result->geometry.sections.push_back({"flame",0,static_cast<std::uint32_t>(result->geometry.indices.size()),std::nullopt});
                return std::shared_ptr<const Resource>(std::move(result));
            }();
            return resource;
        }();
        ResidentFlatDraw draw;draw.resource=mesh;draw.section=0;draw.first_index=0;
        draw.index_count=static_cast<std::uint32_t>(mesh->geometry.indices.size());draw.transform=transform;
        draw.color={1,0.4f,0.08f,1};draw.emission=light.intensity*8.0f;
        output=std::move(draw);return true;
    } catch (...) {return false;}
}

LightingResult BuildVerifiedLighting(const Snapshot& snapshot,VerifiedLighting& output) {
    if (snapshot.light_coverage!=LightCoverage::Complete) return LightingResult::UnknownCoverage;
    if (snapshot.lights.size()>1024) return LightingResult::TooManyLights;
    const auto& conventions=snapshot.conventions;
    if (!ValidProvenance(snapshot.provenance)) return LightingResult::InvalidProvenance;
    if (!snapshot.map_epoch || !snapshot.device_epoch || !snapshot.continuity || snapshot.map.empty() ||
        !Digest(snapshot.map_sha256) || (conventions.length_unit!=LengthUnit::T3Unit && conventions.length_unit!=LengthUnit::Meter) ||
        conventions.axes!=Axes::T3Xyz || conventions.up_axis!=UpAxis::Z || !conventions.meters_per_unit ||
        !std::isfinite(*conventions.meters_per_unit) || *conventions.meters_per_unit<=0 ||
        (conventions.length_unit==LengthUnit::Meter && *conventions.meters_per_unit!=1)) return LightingResult::InvalidUnits;
    VerifiedLighting result;result.map_epoch=snapshot.map_epoch;result.device_epoch=snapshot.device_epoch;
    result.continuity=snapshot.continuity;result.frame_id=snapshot.frame_id;result.provenance=snapshot.provenance;
    result.render_pass=snapshot.render_pass;result.stage=snapshot.stage;
    result.map=snapshot.map;result.map_sha256=snapshot.map_sha256;result.camera=snapshot.camera;result.conventions=conventions;
    result.meters_per_unit=*conventions.meters_per_unit;
    if (!CameraPosition(snapshot.camera.view,result.camera_position)) return LightingResult::InvalidCamera;
    result.observations=snapshot.lights;
    std::unordered_set<std::string> ids;
    for (const auto& light:snapshot.lights) {
        if (!ids.insert(light.id).second) return LightingResult::Incomplete;
        const auto readiness=ClassifyVisualLight(light);
        if (readiness==LightReadiness::Incomplete) return LightingResult::Incomplete;
        if (readiness==LightReadiness::Disabled) continue;
        if (result.lights.size()>=kMaxRenderLights) return LightingResult::TooManyLights;
        const double radius=double(*light.influence_radius.value)*result.meters_per_unit;
        if (!std::isfinite(radius) || radius<=0 || radius>1000000) return LightingResult::InvalidUnits;
        RenderLight converted;converted.id=light.id;converted.generation=light.generation;converted.kind=*light.kind.value;
        converted.position=*light.position.value;converted.color_linear=*light.color_linear.value;
        converted.intensity=*light.intensity.value;converted.influence_radius_m=static_cast<float>(radius);
        converted.source_radius_m=*light.source_radius_m.value;
        if (converted.kind==LightKind::Spot) {
            converted.direction=*light.direction.value;converted.cos_inner=*light.cos_inner.value;converted.cos_outer=*light.cos_outer.value;
        }
        result.lights.push_back(std::move(converted));
    }
    output=std::move(result);return LightingResult::Ready;
}
bool LightingMatchesSnapshot(const Snapshot& snapshot,const VerifiedLighting& lighting) noexcept {
    const auto& a=snapshot.camera;const auto& b=lighting.camera;
    return snapshot.map_epoch==lighting.map_epoch && snapshot.device_epoch==lighting.device_epoch &&
        snapshot.continuity==lighting.continuity && snapshot.frame_id==lighting.frame_id &&
        snapshot.render_pass==lighting.render_pass && snapshot.stage==lighting.stage &&
        snapshot.light_coverage==LightCoverage::Complete && snapshot.lights==lighting.observations &&
        snapshot.map==lighting.map && snapshot.map_sha256==lighting.map_sha256 &&
        snapshot.provenance.origin==lighting.provenance.origin && snapshot.provenance.session_id==lighting.provenance.session_id &&
        snapshot.provenance.capture_sha256==lighting.provenance.capture_sha256 &&
        snapshot.conventions.length_unit==lighting.conventions.length_unit &&
        snapshot.conventions.meters_per_unit==lighting.conventions.meters_per_unit &&
        snapshot.conventions.meters_per_unit && *snapshot.conventions.meters_per_unit==lighting.meters_per_unit &&
        snapshot.conventions.axes==lighting.conventions.axes && snapshot.conventions.up_axis==lighting.conventions.up_axis &&
        snapshot.conventions.handedness==lighting.conventions.handedness && snapshot.conventions.winding==lighting.conventions.winding &&
        snapshot.conventions.uv_origin==lighting.conventions.uv_origin && snapshot.conventions.clip_depth==lighting.conventions.clip_depth &&
        a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height && a.viewport_source==b.viewport_source &&
        std::memcmp(&a.min_depth,&b.min_depth,sizeof(float))==0 && std::memcmp(&a.max_depth,&b.max_depth,sizeof(float))==0 &&
        std::memcmp(a.view.data(),b.view.data(),sizeof(float)*a.view.size())==0 &&
        std::memcmp(a.projection.data(),b.projection.data(),sizeof(float)*a.projection.size())==0;
}
} // namespace t3render::live
