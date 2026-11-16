#pragma once

// Deterministic, bounded one-bounce diffuse preview from submitted world geometry.
// This CPU bake is camera-independent; it is not a real-time GI implementation.
#include "scene_pbr_packet.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>

namespace t3render::world_gi {
struct Vec {
    float x = 0, y = 0, z = 0;
    float operator[](unsigned i) const { return i == 0 ? x : i == 1 ? y : z; }
};
static_assert(sizeof(Vec) == 12, "unexpected irradiance vertex layout");
inline Vec operator+(Vec a, Vec b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec operator-(Vec a, Vec b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec operator*(Vec a, float b) { return {a.x*b, a.y*b, a.z*b}; }
inline Vec operator*(Vec a, Vec b) { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
inline float Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
inline Vec Unit(Vec a) { const float d = Dot(a,a); return d > 1e-20f ? a*(1/std::sqrt(d)) : Vec{}; }
inline Vec Position(const scene::Vertex& v, float scale) { return {v.x*scale, v.y*scale, v.z*scale}; }
inline Vec Normal(const scene::Vertex& v) { return {v.nx,v.ny,v.nz}; }
inline float Linear(unsigned char c) {
    const float s = c/255.0f;
    return s <= .04045f ? s/12.92f : std::pow((s+.055f)/1.055f, 2.4f);
}

inline Vec Albedo(const scene::Texture& texture, float u, float v) {
    const auto& mip = texture.mips.front();
    u -= std::floor(u); v -= std::floor(v);
    const float x = u*mip.width-.5f, y = v*mip.height-.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x-x0, fy = y-y0;
    const auto fetch = [&](int px, int py) {
        const auto wrap = [](int n, int size) { n %= size; return n < 0 ? n+size : n; };
        const std::size_t offset = (std::size_t(wrap(py, static_cast<int>(mip.height)))*mip.width +
                                    wrap(px, static_cast<int>(mip.width)))*4;
        return Vec{Linear(mip.rgba[offset]), Linear(mip.rgba[offset+1]), Linear(mip.rgba[offset+2])};
    };
    return (fetch(x0,y0)*(1-fx)+fetch(x0+1,y0)*fx)*(1-fy) +
           (fetch(x0,y0+1)*(1-fx)+fetch(x0+1,y0+1)*fx)*fy;
}

struct Triangle {
    Vec p[3], n[3], lower, upper, center;
    float u[3]{}, v[3]{}, area = 0, metallic = 0;
    Vec emission;
    std::uint32_t texture = 0, light_mask = 0, first_vertex = 0;
    bool surface = false, bounce_blocker = false;
};
struct Node { Vec lower, upper; std::uint32_t first = 0, count = 0, left = 0, right = 0; };

class Scene {
    std::vector<Triangle> triangles_;
    std::vector<std::uint32_t> blockers_;
    std::vector<Node> nodes_;
    static Vec Min(Vec a, Vec b) { return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)}; }
    static Vec Max(Vec a, Vec b) { return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)}; }
    std::uint32_t Build(std::uint32_t first, std::uint32_t count) {
        const float inf = std::numeric_limits<float>::infinity();
        Vec lo{inf,inf,inf}, hi{-inf,-inf,-inf}, clo=lo, chi=hi;
        for (std::uint32_t i=first; i<first+count; ++i) {
            const auto& t=triangles_[blockers_[i]];
            lo=Min(lo,t.lower); hi=Max(hi,t.upper); clo=Min(clo,t.center); chi=Max(chi,t.center);
        }
        const auto id=static_cast<std::uint32_t>(nodes_.size()); nodes_.push_back({lo,hi,first,count,0,0});
        if (count <= 8) return id;
        const Vec span=chi-clo; const unsigned axis=span.x>=span.y && span.x>=span.z ? 0 : span.y>=span.z ? 1 : 2;
        const std::uint32_t left_count=count/2;
        auto begin=blockers_.begin()+first, middle=begin+left_count, end=begin+count;
        std::nth_element(begin,middle,end,[&](std::uint32_t a,std::uint32_t b) {
            const float x=triangles_[a].center[axis], y=triangles_[b].center[axis];
            return x == y ? a < b : x < y;
        });
        const auto left=Build(first,left_count), right=Build(first+left_count,count-left_count);
        nodes_[id].count=0; nodes_[id].left=left; nodes_[id].right=right; return id;
    }
    static bool Box(const Node& box, Vec p, Vec d, float distance) {
        float tmin=0, tmax=distance;
        for (unsigned k=0; k<3; ++k) {
            if (std::fabs(d[k]) < 1e-10f) {
                if (p[k] < box.lower[k] || p[k] > box.upper[k]) return false;
            } else {
                float a=(box.lower[k]-p[k])/d[k], b=(box.upper[k]-p[k])/d[k];
                if (a>b) std::swap(a,b);
                tmin=std::max(tmin,a); tmax=std::min(tmax,b);
                if (tmin>tmax) return false;
            }
        }
        return true;
    }
    static bool Hit(const Triangle& tri, Vec p, Vec d, float distance) {
        const Vec e1=tri.p[1]-tri.p[0], e2=tri.p[2]-tri.p[0], h=Cross(d,e2);
        const float det=Dot(e1,h);
        if (std::fabs(det)<1e-9f) return false;
        const float inv=1/det; const Vec s=p-tri.p[0]; const float u=inv*Dot(s,h);
        if (u<0 || u>1) return false;
        const Vec q=Cross(s,e1); const float v=inv*Dot(d,q);
        if (v<0 || u+v>1) return false;
        const float t=inv*Dot(e2,q);
        return t>.001f && t<distance-.001f;
    }
public:
    bool Add(const pbr::Frame& frame, std::uint32_t first, const pbr::Draw* draw,
             std::uint32_t mask, bool surface) {
        if (std::size_t(first)+3 > frame.vertices.size()) return false;
        Triangle t; t.first_vertex=first; t.surface=surface; t.light_mask=mask;
        if (draw) {
            t.texture=draw->textures[0]; t.metallic=draw->metallic;
            t.emission={draw->emission[0],draw->emission[1],draw->emission[2]};
        }
        for (unsigned k=0; k<3; ++k) {
            const auto& v=frame.vertices[first+k];
            t.p[k]=Position(v,frame.meters_per_unit); t.n[k]=Normal(v); t.u[k]=v.u; t.v[k]=v.v;
        }
        t.area=.5f*std::sqrt(Dot(Cross(t.p[1]-t.p[0],t.p[2]-t.p[0]),
                                  Cross(t.p[1]-t.p[0],t.p[2]-t.p[0])));
        if (!std::isfinite(t.area)) return false;
        if (t.area < 1e-10f) return true;
        t.lower=Min(t.p[0],Min(t.p[1],t.p[2])); t.upper=Max(t.p[0],Max(t.p[1],t.p[2]));
        t.center=(t.p[0]+t.p[1]+t.p[2])*(1.0f/3);
        // Emission alone does not make an opaque shadow caster transparent.
        t.bounce_blocker=!surface || t.light_mask!=0 || Dot(t.emission,t.emission)==0;
        const auto id=static_cast<std::uint32_t>(triangles_.size()); triangles_.push_back(t);
        if (t.bounce_blocker || t.light_mask) blockers_.push_back(id);
        return true;
    }
    void Finish() { if (!blockers_.empty()) Build(0,static_cast<std::uint32_t>(blockers_.size())); }
    const std::vector<Triangle>& Triangles() const { return triangles_; }
    bool Blocked(Vec from, Vec to, std::uint32_t mask, bool bounce, std::uint32_t ignore_a, std::uint32_t ignore_b,
                 const std::vector<std::uint32_t>* incident = nullptr) const {
        if (nodes_.empty()) return false;
        const Vec delta=to-from; const float length=std::sqrt(Dot(delta,delta));
        if (length<=.002f) return false;
        const Vec direction=delta*(1/length);
        std::array<std::uint32_t,64> stack{}; unsigned size=1; stack[0]=0;
        while (size) {
            const auto& node=nodes_[stack[--size]];
            if (!Box(node,from,direction,length)) continue;
            if (node.count) {
                for (std::uint32_t i=node.first; i<node.first+node.count; ++i) {
                    const auto id=blockers_[i]; const auto& tri=triangles_[id];
                    if (id==ignore_a || id==ignore_b || (bounce ? !tri.bounce_blocker : !(tri.light_mask & mask)) ||
                        (incident && std::find(incident->begin(),incident->end(),id)!=incident->end())) continue;
                    if (Hit(tri,from,direction,length)) return true;
                }
            } else {
                if (size+2>stack.size()) return true; // fail closed if an unexpected tree exceeds the bound
                stack[size++]=node.left; stack[size++]=node.right;
            }
        }
        return false;
    }
};

struct Surfel { Vec point, normal, radiance; float weight = 0; std::uint32_t triangle = 0; };
using VertexKey = std::array<std::uint32_t,6>;
struct KeyHash {
    std::size_t operator()(const VertexKey& key) const {
        std::size_t value=1469598103u;
        for (auto part : key) value=(value^part)*16777619u;
        return value;
    }
};
struct Receiver { Vec point, normal; std::vector<std::uint32_t> triangles, vertices; };
struct Statistics { std::size_t receivers = 0, surfels = 0; unsigned workers = 0; };
constexpr unsigned kSamples = 128;
constexpr std::uint64_t kMaxVertexSamples = 16000000;
constexpr float kPi = 3.14159265358979323846f, kOffset = .003f;

inline bool Bake(const pbr::Frame& frame, const std::vector<std::uint32_t>& draw_ids,
                 const std::vector<std::uint32_t>& caster_ids, std::vector<Vec>& irradiance, std::string& error,
                 Statistics* statistics = nullptr) {
    Scene scene;
    std::uint64_t vertex_count=0;
    for (auto id : draw_ids) {
        if (id>=frame.draws.size()) { error="invalid GI draw"; return false; }
        vertex_count+=frame.draws[id].vertex_count;
    }
    if (vertex_count*kSamples > kMaxVertexSamples) { error="world GI exceeds vertex/sample budget"; return false; }
    for (auto id : draw_ids) {
        const auto& draw=frame.draws[id];
        const auto mask=frame.shadow_size ? draw.shadow_mask : (1u<<frame.lights.size())-1u;
        for (std::uint32_t n=0; n<draw.vertex_count; n+=3)
            if (!scene.Add(frame,draw.first_vertex+n,&draw,mask,true)) { error="invalid GI surface"; return false; }
    }
    for (auto id : caster_ids) {
        if (id>=frame.offline_casters.size()) { error="invalid GI caster"; return false; }
        const auto& caster=frame.offline_casters[id];
        for (std::uint32_t n=0; n<caster.vertex_count; n+=3)
            if (!scene.Add(frame,caster.first_vertex+n,nullptr,caster.shadow_mask,false)) { error="invalid GI caster"; return false; }
    }
    scene.Finish(); const auto& triangles=scene.Triangles();
    std::vector<std::uint32_t> diffuse, emissive;
    float diffuse_area=0, emissive_area=0;
    for (std::uint32_t i=0; i<triangles.size(); ++i) {
        const auto& tri=triangles[i]; if (!tri.surface) continue;
        if (Dot(tri.emission,tri.emission)>0) { emissive.push_back(i); emissive_area+=tri.area; }
        else { diffuse.push_back(i); diffuse_area+=tri.area; }
    }
    if (!std::isfinite(diffuse_area) || !std::isfinite(emissive_area)) { error="invalid GI surface area"; return false; }
    std::vector<Surfel> surfels; surfels.reserve(kSamples);
    const auto sample = [&](const std::vector<std::uint32_t>& ids, float area, unsigned count) {
        if (ids.empty() || !count || area<=0) return;
        std::size_t index=0; float cumulative=triangles[ids[0]].area;
        for (unsigned i=0; i<count; ++i) {
            const float target=(i+.5f)*area/count;
            while (index+1<ids.size() && cumulative<target) cumulative+=triangles[ids[++index]].area;
            const auto id=ids[index]; const auto& tri=triangles[id];
            const float q=(i+.5f)*.61803398875f, r=(i+.5f)*.41421356237f;
            const float root=std::sqrt(q-std::floor(q)), b1=root*(1-(r-std::floor(r))), b2=root*(r-std::floor(r));
            const float b0=1-b1-b2;
            const Vec point=tri.p[0]*b0+tri.p[1]*b1+tri.p[2]*b2;
            const Vec normal=Unit(tri.n[0]*b0+tri.n[1]*b1+tri.n[2]*b2);
            Vec radiance=tri.emission;
            if (Dot(normal,normal)>0 && tri.metallic<1) {
                const Vec albedo=Albedo(frame.textures[tri.texture],tri.u[0]*b0+tri.u[1]*b1+tri.u[2]*b2,
                                        tri.v[0]*b0+tri.v[1]*b1+tri.v[2]*b2);
                for (std::uint32_t n=0; n<frame.lights.size(); ++n) {
                    const auto& light=frame.lights[n];
                    const Vec target_light=Vec{light.position[0],light.position[1],light.position[2]}*frame.meters_per_unit;
                    const Vec delta=target_light-point; const float d2=Dot(delta,delta);
                    if (d2<1e-10f) continue;
                    const Vec direction=delta*(1/std::sqrt(d2)); const float cosine=std::max(0.0f,Dot(normal,direction));
                    if (cosine<=0 || light.intensity<=0) continue;
                    float cone=1; const Vec axis{light.direction[0],light.direction[1],light.direction[2]};
                    if (Dot(axis,axis)>0) {
                        const float x=std::clamp((Dot(axis,direction*-1)-light.cos_outer)/(light.cos_inner-light.cos_outer),0.0f,1.0f);
                        cone=x*x*(3-2*x);
                    }
                    if (cone<=0 || scene.Blocked(point+normal*kOffset,target_light,1u<<n,false,id,id)) continue;
                    const float power=light.intensity*cosine*cone/std::max(d2,light.radius_m*light.radius_m);
                    radiance=radiance+(albedo*Vec{light.color[0],light.color[1],light.color[2]})*(power*(1-tri.metallic)*.96f/kPi);
                }
            }
            surfels.push_back({point,normal,radiance,area/count,id});
        }
    };
    const unsigned emissive_samples=emissive.empty() ? 0u : diffuse.empty() ? kSamples : kSamples/4;
    sample(diffuse,diffuse_area,kSamples-emissive_samples); sample(emissive,emissive_area,emissive_samples);
    irradiance.assign(frame.vertices.size(),Vec{});
    std::unordered_map<VertexKey,std::size_t,KeyHash> index;
    std::vector<Receiver> receivers;
    for (std::uint32_t id=0; id<triangles.size(); ++id) {
        const auto& triangle=triangles[id]; if (!triangle.surface) continue;
        for (unsigned k=0; k<3; ++k) {
            const auto vertex=triangle.first_vertex+k; const auto& source=frame.vertices[vertex];
            VertexKey key{}; std::memcpy(key.data(),&source.x,3*sizeof(float)); std::memcpy(key.data()+3,&source.nx,3*sizeof(float));
            auto found=index.find(key);
            if (found==index.end()) {
                const auto next=receivers.size(); index.emplace(key,next);
                receivers.push_back({triangle.p[k],Unit(triangle.n[k]),{}, {}}); found=index.find(key);
            }
            auto& receiver=receivers[found->second]; receiver.triangles.push_back(id); receiver.vertices.push_back(vertex);
        }
    }
    std::atomic<bool> finite{true};
    const unsigned workers=receivers.size()<512 ? 1u : std::max(1u,std::min(8u,std::thread::hardware_concurrency()));
    const auto compute = [&](unsigned worker) {
        const auto first=receivers.size()*worker/workers, last=receivers.size()*(worker+1)/workers;
        for (auto i=first; i<last; ++i) {
            const auto& receiver=receivers[i]; const Vec point=receiver.point, normal=receiver.normal; Vec sum{};
            for (const auto& source : surfels) {
                if (Dot(source.radiance,source.radiance)==0 ||
                    std::find(receiver.triangles.begin(),receiver.triangles.end(),source.triangle)!=receiver.triangles.end()) continue;
                const Vec delta=source.point-point; const float d2=Dot(delta,delta);
                if (d2<.0025f || d2>225) continue;
                const Vec direction=delta*(1/std::sqrt(d2));
                const float a=std::max(0.0f,Dot(normal,direction)), b=std::max(0.0f,Dot(source.normal,direction*-1));
                if (a<=0 || b<=0 || scene.Blocked(point+normal*kOffset,source.point+source.normal*kOffset,0,true,
                                                  UINT32_MAX,source.triangle,&receiver.triangles)) continue;
                sum=sum+source.radiance*(std::min(source.weight/d2,kPi)*a*b);
            }
            if (!std::isfinite(sum.x) || !std::isfinite(sum.y) || !std::isfinite(sum.z)) { finite=false; continue; }
            const Vec result{std::min(sum.x,65504.0f),std::min(sum.y,65504.0f),std::min(sum.z,65504.0f)};
            for (auto vertex : receiver.vertices) irradiance[vertex]=result;
        }
    };
    std::vector<std::thread> threads; threads.reserve(workers-1);
    try {
        for (unsigned i=1; i<workers; ++i) threads.emplace_back(compute,i);
    } catch (...) {
        for (auto& thread : threads) thread.join();
        error="cannot start world GI workers"; return false;
    }
    compute(0);
    for (auto& thread : threads) thread.join();
    if (!finite) { error="non-finite world GI"; return false; }
    if (statistics) *statistics={receivers.size(),surfels.size(),workers};
    return true;
}
} // namespace t3render::world_gi
