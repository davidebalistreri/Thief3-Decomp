#include "scene_live_lighting.hpp"

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
