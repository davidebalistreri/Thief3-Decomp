#include "scene_live_materials.hpp"
#include "scene_packet.hpp"

#include <algorithm>
#include <cmath>

namespace t3render::live {
namespace {
constexpr double kScale = 0.01905;
bool EqualAscii(const std::string& a, const char* b) {
    std::size_t i=0;
    for (;i<a.size() && b[i];++i) {
        const auto lower=[](unsigned char c){return c>='A' && c<='Z' ? c+('a'-'A') : c;};
        if (lower(static_cast<unsigned char>(a[i]))!=lower(static_cast<unsigned char>(b[i]))) return false;
    }
    return i==a.size() && !b[i];
}
std::array<double,3> Local(const BedroomAnchor& anchor, const Vec3& position) {
    std::array<double,3> p{}, t3{};
    for (unsigned i=0;i<3;++i) p[i]=double(position[i])-anchor.origin[i];
    for (unsigned j=0;j<3;++j) for (unsigned i=0;i<3;++i) t3[j]+=p[i]*anchor.inverse[i*3+j];
    return {t3[0]*kScale,t3[2]*kScale,t3[1]*kScale};
}
} // namespace

bool FindBedroomAnchor(const Snapshot& snapshot, BedroomAnchor& output) {
    const auto& conventions=snapshot.conventions;
    if (snapshot.map!="SouthQuarter1_int1" || snapshot.map_sha256.size()!=64 ||
        conventions.length_unit!=LengthUnit::T3Unit || conventions.axes!=Axes::T3Xyz ||
        conventions.up_axis!=UpAxis::Z || !conventions.meters_per_unit ||
        !std::isfinite(*conventions.meters_per_unit) ||
        std::fabs(double(*conventions.meters_per_unit)-kScale)>1e-9) return false;
    const auto actor="actor:"+snapshot.map+":"+snapshot.map_sha256+":StaticMeshActor__1520";
    const ResidentMesh* selected=nullptr;
    for (const auto& mesh:snapshot.meshes) if (mesh.selection.instance_id==actor) {
        if (selected) return false;
        selected=&mesh;
    }
    if (!selected || !selected->resource || selected->selection.sections.empty() ||
        !selected->selection.generation ||
        (selected->selection.binding!=BindingEvidence::Candidate && selected->selection.binding!=BindingEvidence::Verified) ||
        selected->resource->kind!=ResourceKind::StaticMesh ||
        selected->resource->source_sha256.size()!=64 ||
        selected->selection.resource_id!=selected->resource->id ||
        !EqualAscii(selected->resource->name,"innbed1a1") ||
        selected->resource->id!="mesh:"+snapshot.map+":"+snapshot.map_sha256+":"+
                               selected->resource->source_sha256+":innbed1a1") return false;
    const auto& m=selected->selection.transform;
    if (!std::all_of(m.begin(),m.end(),[](float x){return std::isfinite(x);}) ||
        std::fabs(m[3])>1e-6f || std::fabs(m[7])>1e-6f || std::fabs(m[11])>1e-6f ||
        std::fabs(m[15]-1)>1e-6f) return false;
    const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
    const std::array<double,9> cof{e*i-f*h,f*g-d*i,d*h-e*g,
                                   c*h-b*i,a*i-c*g,b*g-a*h,
                                   b*f-c*e,c*d-a*f,a*e-b*d};
    const double det=a*cof[0]+b*cof[1]+c*cof[2];
    if (!std::isfinite(det) || std::fabs(det)<1e-12) return false;
    BedroomAnchor result;result.origin={m[12],m[13],m[14]};
    for (unsigned row=0;row<3;++row) for (unsigned col=0;col<3;++col) {
        result.inverse[row*3+col]=cof[col*3+row]/det;
        if (!std::isfinite(result.inverse[row*3+col])) return false;
    }
    output=result;return true;
}

bool PartitionBedroomMaterial(const BedroomAnchor& anchor, const Resource& bsp,
                              std::uint32_t section_index, const std::string& material_name,
                              std::vector<BedroomMaterialRun>& output, bool identify_head_wall) {
    if (bsp.kind!=ResourceKind::BspBlock || section_index>=bsp.geometry.sections.size()) return false;
    const auto& g=bsp.geometry;const auto& section=g.sections[section_index];
    if (!section.index_count || section.index_count%3 || section.index_count>scene::kMaxTriangles*3 ||
        std::uint64_t{section.first_index}+section.index_count>g.indices.size() ||
        std::uint64_t{section.first_index}+section.index_count>UINT32_MAX) return false;
    const bool wall=EqualAscii(material_name,"STMwall512D_D"), floor=EqualAscii(material_name,"HSEfloorB_D");
    if (!wall && !floor) {
        output={{section.first_index,section.index_count,std::nullopt}};return true;
    }
    const std::array<double,3> low=wall ? std::array<double,3>{-.25,-.10,-2.10} : std::array<double,3>{-6.0,-.08,-1.6};
    const std::array<double,3> high=wall ? std::array<double,3>{4.85,4.10,3.35} : std::array<double,3>{5.0,.08,3.2};
    std::vector<BedroomMaterialRun> runs;
    for (std::uint32_t offset=0;offset<section.index_count;offset+=3) {
        std::array<std::array<double,3>,3> point{};
        for (unsigned n=0;n<3;++n) {
            const auto index=g.indices[section.first_index+offset+n];
            if (index>=g.positions.size()) return false;
            point[n]=Local(anchor,g.positions[index]);
            if (!std::all_of(point[n].begin(),point[n].end(),[](double v){return std::isfinite(v);})) return false;
        }
        std::array<double,3> ab{},ac{},center{};
        for (unsigned n=0;n<3;++n) {
            ab[n]=point[1][n]-point[0][n];ac[n]=point[2][n]-point[0][n];
            center[n]=(point[0][n]+point[1][n]+point[2][n])/3;
        }
        const std::array<double,3> face{ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
        const double length=std::sqrt(face[0]*face[0]+face[1]*face[1]+face[2]*face[2]);
        bool inside=std::isfinite(length) && length>1e-12;
        for (unsigned n=0;n<3;++n) inside=inside && center[n]>=low[n] && center[n]<high[n];
        const bool facing=wall ? std::fabs(face[1])<length*.1 : face[1]<-length*.99;
        const std::optional<std::string> replacement=inside && facing ?
            std::optional<std::string>(wall ? "remaster:material:stone_wall_v2" : "remaster:material:wood_floor_v2") : std::nullopt;
        // This plane is the bedroom wall immediately behind the headboard,
        // expressed in the selected bed's local metres, not world coordinates.
        const bool head_wall=identify_head_wall && wall && replacement &&
            std::fabs(face[0])>length*.99 && center[0]>=-.25 && center[0]<.02;
        if (!runs.empty() && runs.back().replacement==replacement && runs.back().head_wall==head_wall)
            runs.back().index_count+=3;
        else {
            if (runs.size()>=scene::kMaxDraws) return false;
            runs.push_back({section.first_index+offset,3,replacement,head_wall});
        }
    }
    output=std::move(runs);return true;
}
} // namespace t3render::live
