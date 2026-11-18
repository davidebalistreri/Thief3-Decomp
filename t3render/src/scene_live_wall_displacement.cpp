#include "scene_live_wall_displacement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace t3render::live {
namespace {
constexpr unsigned kSteps=24;
constexpr std::size_t kMaxBytes=32u*1024u*1024u, kMaxEntries=128;
constexpr float kMetersPerUnit=0.01905f, kDepthMeters=0.05f;
constexpr char kMapHash[]="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";

bool Unit(Vec3& v) {
    const double length=std::sqrt(double(v[0])*v[0]+double(v[1])*v[1]+double(v[2])*v[2]);
    if (!std::isfinite(length) || length<1e-10) return false;
    for (float& x:v) x=static_cast<float>(x/length);
    return true;
}
float Height(const scene::Mip& mip, float u, float v) {
    const auto wrap=[](int x,int size) {x%=size;return x<0 ? x+size : x;};
    const double x=(double(u)-std::floor(double(u)))*mip.width-.5;
    const double y=(double(v)-std::floor(double(v)))*mip.height-.5;
    const int x0=static_cast<int>(std::floor(x)),y0=static_cast<int>(std::floor(y));
    const float fx=static_cast<float>(x-x0),fy=static_cast<float>(y-y0);
    const auto sample=[&](int sx,int sy) {
        return mip.rgba[(std::size_t(wrap(sy,static_cast<int>(mip.height)))*mip.width+
                         wrap(sx,static_cast<int>(mip.width)))*4]/255.0f;
    };
    const float a=sample(x0,y0)*(1-fx)+sample(x0+1,y0)*fx;
    const float b=sample(x0,y0+1)*(1-fx)+sample(x0+1,y0+1)*fx;
    return a*(1-fy)+b*fy;
}

float Displacement(const scene::Mip& height,const Vec2& uv,const Vec3& point,const BedroomAnchor& anchor) {
    std::array<double,3> local{};
    for (unsigned col=0;col<3;++col) for (unsigned row=0;row<3;++row)
        local[col]+=(double(point[row])-anchor.origin[row])*anchor.inverse[row*3+col]*kMetersPerUnit;
    // Fade the displaced wall into the undisplaced room edges.
    const double distance=std::min({local[1]+2.0193,3.1623-local[1],local[2],3.9624-local[2]});
    const float edge=static_cast<float>(std::clamp(distance/.12,0.0,1.0));
    return (1-Height(height,uv[0],uv[1]))*kDepthMeters/kMetersPerUnit*edge*edge*(3-2*edge);
}

// Different source triangles duplicate their boundary vertices. Derive a
// normal from the same continuous height field on either side rather than
// averaging only the faces belonging to one source triangle.
bool HeightNormal(const scene::Mip& height,const Vec2& uv,const Vec3& point,
                  const Vec3& tangent_u,const Vec3& tangent_v,const Vec3& interior,
                  const BedroomAnchor& anchor,Vec3& output) {
    const auto slope=[&](unsigned axis,const Vec3& tangent) {
        const float step=1.0f/(axis==0 ? height.width : height.height);
        Vec2 before_uv=uv,after_uv=uv;before_uv[axis]-=step;after_uv[axis]+=step;
        if (before_uv[axis]==uv[axis] || after_uv[axis]==uv[axis])
            return std::numeric_limits<float>::quiet_NaN();
        Vec3 before=point,after=point;
        for (unsigned i=0;i<3;++i) {before[i]-=tangent[i]*step;after[i]+=tangent[i]*step;}
        return (Displacement(height,after_uv,after,anchor)-Displacement(height,before_uv,before,anchor))/(2*step);
    };
    const float du=slope(0,tangent_u),dv=slope(1,tangent_v);
    if (!std::isfinite(du) || !std::isfinite(dv)) return false;
    Vec3 u{},v{};
    for (unsigned i=0;i<3;++i) {u[i]=tangent_u[i]-interior[i]*du;v[i]=tangent_v[i]-interior[i]*dv;}
    Vec3 normal{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
    if (normal[0]*interior[0]+normal[1]*interior[1]+normal[2]*interior[2]<0)
        for (auto& x:normal) x=-x;
    if (!Unit(normal)) return false;
    output=normal;return true;
}

std::shared_ptr<const Resource> Generate(const ResidentFlatDraw& draw,const scene::Mip& height,
                                         const Vec3& interior,const BedroomAnchor& anchor,
                                         std::uint64_t serial,std::size_t& bytes) {
    const auto& g=draw.resource->geometry;
    if (g.positions.empty() || g.normals.size()!=g.positions.size() || g.uv0.size()!=g.positions.size() ||
        !draw.index_count || draw.index_count%3 ||
        std::uint64_t{draw.first_index}+draw.index_count>g.indices.size()) return {};
    const std::size_t count=draw.index_count/3;
    constexpr std::size_t vertices_per=(kSteps+1)*(kSteps+2)/2, indices_per=kSteps*kSteps*3;
    if (count>scene::kMaxTriangles/(kSteps*kSteps) || count>1000000/vertices_per) return {};
    auto result=std::make_shared<Resource>();result->kind=ResourceKind::BspBlock;
    result->id="preview:wall-displacement:"+std::to_string(serial);
    result->name="bedroom_head_wall_displaced";result->source_sha256=draw.resource->source_sha256;
    auto& out=result->geometry;
    out.positions.reserve(count*vertices_per);out.normals.reserve(count*vertices_per);
    out.uv0.reserve(count*vertices_per);out.indices.reserve(count*indices_per);
    for (std::size_t tri=0;tri<count;++tri) {
        std::array<Vec3,3> p{};std::array<Vec2,3> uv{};
        for (unsigned k=0;k<3;++k) {
            const auto index=g.indices[draw.first_index+tri*3+k];
            if (index>=g.positions.size()) return {};
            p[k]=g.positions[index];uv[k]=g.uv0[index];
            for (float x:p[k]) if (!std::isfinite(x) || std::fabs(x)>998000) return {};
            for (float x:uv[k]) if (!std::isfinite(x) || std::fabs(x)>1000000) return {};
        }
        const double du1=double(uv[1][0])-uv[0][0],dv1=double(uv[1][1])-uv[0][1];
        const double du2=double(uv[2][0])-uv[0][0],dv2=double(uv[2][1])-uv[0][1];
        const double determinant=du1*dv2-dv1*du2;
        Vec3 tangent_u{},tangent_v{};
        bool have_basis=std::isfinite(determinant) && std::fabs(determinant)>1e-12;
        if (have_basis) for (unsigned axis=0;axis<3;++axis) {
            const double a=double(p[1][axis])-p[0][axis],b=double(p[2][axis])-p[0][axis];
            const double u=(a*dv2-b*dv1)/determinant,v=(b*du1-a*du2)/determinant;
            if (!std::isfinite(u) || !std::isfinite(v) || std::fabs(u)>1e7 || std::fabs(v)>1e7) {
                have_basis=false;break;
            }
            tangent_u[axis]=static_cast<float>(u);tangent_v[axis]=static_cast<float>(v);
        }
        const auto base=static_cast<std::uint32_t>(out.positions.size());
        std::array<bool,vertices_per> height_normals{};
        std::size_t vertex_in_triangle=0;
        for (unsigned i=0;i<=kSteps;++i) for (unsigned j=0;j<=kSteps-i;++j) {
            const float a=float(i)/kSteps,b=float(j)/kSteps;
            Vec2 q{};Vec3 point{};
            for (unsigned axis=0;axis<2;++axis) q[axis]=uv[0][axis]+(uv[1][axis]-uv[0][axis])*a+(uv[2][axis]-uv[0][axis])*b;
            for (unsigned axis=0;axis<3;++axis)
                point[axis]=p[0][axis]+(p[1][axis]-p[0][axis])*a+(p[2][axis]-p[0][axis])*b;
            Vec3 normal{};
            height_normals[vertex_in_triangle++]=have_basis &&
                HeightNormal(height,q,point,tangent_u,tangent_v,interior,anchor,normal);
            const float displacement=Displacement(height,q,point,anchor);
            for (unsigned axis=0;axis<3;++axis) point[axis]-=interior[axis]*displacement;
            out.positions.push_back(point);out.uv0.push_back(q);out.normals.push_back(normal);
        }
        const auto index=[&](unsigned i,unsigned j) {return base+i*(2*kSteps+3-i)/2+j;};
        for (unsigned i=0;i<kSteps;++i) for (unsigned j=0;j<kSteps-i;++j) {
            out.indices.insert(out.indices.end(),{index(i,j),index(i+1,j),index(i,j+1)});
            if (i+j+1<kSteps)
                out.indices.insert(out.indices.end(),{index(i+1,j),index(i+1,j+1),index(i,j+1)});
        }
        for (std::size_t at=out.indices.size()-indices_per;at<out.indices.size();at+=3) {
            const auto a=out.indices[at],b=out.indices[at+1],c=out.indices[at+2];
            const Vec3 u{out.positions[b][0]-out.positions[a][0],out.positions[b][1]-out.positions[a][1],out.positions[b][2]-out.positions[a][2]};
            const Vec3 v{out.positions[c][0]-out.positions[a][0],out.positions[c][1]-out.positions[a][1],out.positions[c][2]-out.positions[a][2]};
            Vec3 normal{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
            if (normal[0]*interior[0]+normal[1]*interior[1]+normal[2]*interior[2]<0)
                for (auto& x:normal) x=-x;
            for (auto vertex:{a,b,c}) if (!height_normals[vertex-base])
                for (unsigned axis=0;axis<3;++axis) out.normals[vertex][axis]+=normal[axis];
        }
        for (std::size_t vertex=base;vertex<out.positions.size();++vertex)
            if (!Unit(out.normals[vertex])) out.normals[vertex]=interior;
    }
    out.sections.push_back({"displaced_head_wall",0,static_cast<std::uint32_t>(out.indices.size()),draw.material_id});
    bytes=out.positions.size()*(sizeof(Vec3)*2+sizeof(Vec2))+out.indices.size()*sizeof(std::uint32_t);
    return result;
}
} // namespace

void WallDisplacementCache::Clear() noexcept {
    entries_.clear();materials_.reset();bytes_=0;map_=device_=continuity_=0;bed_transform_={};
}

bool WallDisplacementCache::Apply(const Snapshot& snapshot, ResidentFlatFrame& frame,
                                  const std::shared_ptr<const BedroomMaterialLibrary>& materials, WallDisplacementStats& stats) noexcept {
    try {
        if (snapshot.map!="SouthQuarter1_int1" || snapshot.map_sha256!=kMapHash ||
            !materials || !materials->wall_height || materials->wall_height_source_sha256.size()!=64 ||
            materials->wall_height->mips.empty() || !snapshot.conventions.meters_per_unit ||
            std::fabs(*snapshot.conventions.meters_per_unit-kMetersPerUnit)>1e-8f ||
            frame.frame_id!=snapshot.frame_id || frame.map_epoch!=snapshot.map_epoch ||
            frame.device_epoch!=snapshot.device_epoch || frame.continuity!=snapshot.continuity ||
            frame.map!=snapshot.map || frame.map_sha256!=snapshot.map_sha256) return false;
        BedroomAnchor anchor;if (!FindBedroomAnchor(snapshot,anchor)) return false;
        const auto actor="actor:"+snapshot.map+":"+snapshot.map_sha256+":StaticMeshActor__1520";
        const ResidentMesh* bed=nullptr;
        for (const auto& mesh:snapshot.meshes) if (mesh.selection.instance_id==actor) {if (bed) return false;bed=&mesh;}
        if (!bed) return false;
        const auto& transform=bed->selection.transform;
        const Matrix expected{1,0,0,0,0,1,0,0,0,0,1,0,3398,1514,272,1};
        for (unsigned i=0;i<16;++i) if (std::fabs(transform[i]-expected[i])>(i>=12 && i<15 ? .1f : 1e-4f)) return false;
        Vec3 interior{transform[0],transform[1],transform[2]};if (!Unit(interior)) return false;
        const auto& height=materials->wall_height->mips.front();
        if (!height.width || height.width>1024 || height.width!=height.height ||
            height.rgba.size()!=std::size_t(height.width)*height.height*4) return false;
        if (map_!=snapshot.map_epoch || device_!=snapshot.device_epoch || continuity_!=snapshot.continuity ||
            materials_!=materials || bed_transform_!=transform) {
            Clear();map_=snapshot.map_epoch;device_=snapshot.device_epoch;
            continuity_=snapshot.continuity;materials_=materials;bed_transform_=transform;
        }
        std::uint64_t total=0,source=0;
        for (const auto& draw:frame.draws) {
            total+=draw.index_count/3;
            if (draw.wall_relief==2) source+=draw.index_count/3;
        }
        if (total>scene::kMaxTriangles || source>scene::kMaxTriangles ||
            source*(kSteps*kSteps-1)>scene::kMaxTriangles-total) return false;
        auto updated=frame.draws;
        const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        for (auto& draw:updated) if (draw.wall_relief==2) {
            if (!draw.resource || draw.resource->kind!=ResourceKind::BspBlock || !draw.material_id ||
                *draw.material_id!="remaster:material:stone_wall_v2" || draw.transform!=identity) return false;
            const auto match=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){
                return e.source==draw.resource && e.first==draw.first_index && e.count==draw.index_count;});
            std::shared_ptr<const Resource> replacement;
            if (match!=entries_.end()) replacement=match->replacement;
            else {
                if (entries_.size()>=kMaxEntries || serial_==std::numeric_limits<std::uint64_t>::max()) return false;
                std::size_t size=0;replacement=Generate(draw,height,interior,anchor,++serial_,size);
                if (!replacement || size>kMaxBytes-bytes_) return false;
                entries_.push_back({draw.resource,replacement,draw.first_index,draw.index_count});bytes_+=size;
            }
            draw.resource=std::move(replacement);draw.section=0;draw.first_index=0;
            draw.index_count=static_cast<std::uint32_t>(draw.resource->geometry.indices.size());
            draw.wall_relief=1;draw.wall_displaced=true;
        }
        frame.draws=std::move(updated);
        stats.source_triangles=static_cast<std::uint32_t>(source);
        stats.displaced_triangles=static_cast<std::uint32_t>(source*kSteps*kSteps);
        return true;
    } catch (...) {return false;}
}
} // namespace t3render::live
