#include "scene_live_framing.hpp"
#include "scene_live_lighting.hpp"
#include "scene_live_materials.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace t3render::live {
namespace {
using Vector4=std::array<double,4>;
Vector4 Multiply(const Vector4& value,const Matrix& matrix) {
    Vector4 result{};
    for (unsigned column=0;column<4;++column)
        for (unsigned row=0;row<4;++row) result[column]+=value[row]*double(matrix[row*4+column]);
    return result;
}
bool ProjectBox(const Resource& resource,const Matrix& transform,const Camera& camera,
                BedroomFramingBounds& output) {
    const auto& positions=resource.geometry.positions;
    if (positions.empty() || positions.size()>50000 || !camera.width || !camera.height ||
        !std::all_of(transform.begin(),transform.end(),[](float v){return std::isfinite(v);}) ||
        !std::all_of(camera.view.begin(),camera.view.end(),[](float v){return std::isfinite(v);}) ||
        !std::all_of(camera.projection.begin(),camera.projection.end(),[](float v){return std::isfinite(v);})) return false;
    std::array<double,3> low{},high{};
    for (unsigned i=0;i<3;++i) low[i]=high[i]=positions[0][i];
    for (const auto& point:positions) for (unsigned i=0;i<3;++i) {
        if (!std::isfinite(point[i]) || std::fabs(point[i])>1000000) return false;
        low[i]=std::min(low[i],double(point[i]));high[i]=std::max(high[i],double(point[i]));
    }
    BedroomFramingBounds result{1e9,1e9,-1e9,-1e9};
    for (unsigned corner=0;corner<8;++corner) {
        Vector4 local{(corner&1) ? high[0] : low[0],(corner&2) ? high[1] : low[1],
                      (corner&4) ? high[2] : low[2],1};
        const auto clip=Multiply(Multiply(Multiply(local,transform),camera.view),camera.projection);
        if (!std::all_of(clip.begin(),clip.end(),[](double v){return std::isfinite(v);}) || clip[3]<=1e-9 ||
            clip[2]<0 || clip[2]>clip[3]) return false;
        const double x=(clip[0]/clip[3]+1)*.5, y=(1-clip[1]/clip[3])*.5;
        if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x)>1e8 || std::fabs(y)>1e8) return false;
        result.left=std::min(result.left,x);result.top=std::min(result.top,y);
        result.right=std::max(result.right,x);result.bottom=std::max(result.bottom,y);
    }
    if (!(result.left<result.right && result.top<result.bottom)) return false;
    output=result;return true;
}
} // namespace

bool BedroomBedFraming(const Snapshot& snapshot,const MeshOverride& override,BedroomFramingBounds& output) noexcept {
    try {
        BedroomAnchor anchor;
        if (!FindBedroomAnchor(snapshot,anchor) || override.map!=snapshot.map ||
            override.map_sha256!=snapshot.map_sha256 || !override.source || !override.replacement ||
            override.instance_id!="actor:"+snapshot.map+":"+snapshot.map_sha256+":StaticMeshActor__1520" ||
            override.replacement->kind!=ResourceKind::StaticMesh) return false;
        for (const auto& mesh:snapshot.meshes) if (mesh.selection.instance_id==override.instance_id) {
            if (mesh.resource!=override.source || mesh.selection.resource_id!=override.source->id) return false;
            return ProjectBox(*override.replacement,mesh.selection.transform,snapshot.camera,output);
        }
        return false;
    } catch (...) {return false;}
}

bool BedroomCandleFraming(const Snapshot& snapshot,const RawLightFrame& raw,BedroomFramingBounds& output,
                          std::uint32_t actor_number) noexcept {
    try {
        std::array<ResidentFlatDraw,2> holder;
        if (!CandlePreviewCandlestick(snapshot,raw,holder,actor_number) || !holder[0].resource ||
            holder[0].resource!=holder[1].resource || holder[0].transform!=holder[1].transform) return false;
        BedroomFramingBounds result;
        if (!ProjectBox(*holder[0].resource,holder[0].transform,snapshot.camera,result)) return false;
        RenderLight light;
        const auto state=RemasterCandleFromRaw(snapshot,raw,light,actor_number);
        if (state==RemasterCandleState::Unknown) return false;
        if (state==RemasterCandleState::On) {
            ResidentFlatDraw flame;BedroomFramingBounds flame_bounds;
            if (!CandlePreviewFlame(snapshot,raw,flame,actor_number) || !flame.resource ||
                !ProjectBox(*flame.resource,flame.transform,snapshot.camera,flame_bounds)) return false;
            result.left=std::min(result.left,flame_bounds.left);result.top=std::min(result.top,flame_bounds.top);
            result.right=std::max(result.right,flame_bounds.right);result.bottom=std::max(result.bottom,flame_bounds.bottom);
        }
        output=result;return true;
    } catch (...) {return false;}
}
} // namespace t3render::live
