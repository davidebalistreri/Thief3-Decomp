// Independent SceneIR material/light preview; no D3D8 render-state replay.
#include "scene_d3d12_common.hpp"
#include "scene_pbr_packet.hpp"
#include "scene_pbr_variants.hpp"
#include "scene_temporal_history.hpp"
#include <bcrypt.h>
#include <cmath>
#include <iostream>
#include <string>

namespace {
using namespace t3render::pbr;
using namespace t3render::dx12;
constexpr DXGI_FORMAT kHdr = DXGI_FORMAT_R16G16B16A16_FLOAT;

bool ReadPacket(const wchar_t* path, Frame& frame, std::uint8_t* digest) {
    FILE* file = _wfopen(path, L"rb");
    if (!file) { std::fprintf(stderr, "cannot open PBR packet\n"); return false; }
    std::vector<std::uint8_t> bytes(t3render::scene::kMaxPacketBytes + 1);
    const std::size_t size = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool error = std::ferror(file) != 0;
    const bool close_error = std::fclose(file) != 0;
    if (error || close_error || !ParsePacket(bytes.data(), size, frame)) {
        std::fprintf(stderr, "invalid PBR packet\n"); return false;
    }
    if (digest) {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD object_size = 0, written = 0;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                                       sizeof(object_size), &written, 0) >= 0 && written == sizeof(object_size) && object_size;
        std::vector<std::uint8_t> object(ok ? object_size : 0);
        if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
        if (ok) ok = BCryptHashData(hash, bytes.data(), static_cast<ULONG>(size), 0) >= 0 &&
                     BCryptFinishHash(hash, digest, 32, 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok) { std::fprintf(stderr, "cannot hash PBR packet\n"); return false; }
    }
    return true;
}

bool ReadVariants(const wchar_t* path, Variants& variants) {
    FILE* file = _wfopen(path, L"rb");
    if (!file) { std::fprintf(stderr, "cannot open PBR variants\n"); return false; }
    bool ok = !_fseeki64(file, 0, SEEK_END);
    const auto length = ok ? _ftelli64(file) : -1;
    ok = ok && length > 0 && static_cast<std::uint64_t>(length) <= kMaxVariantBytes && !_fseeki64(file, 0, SEEK_SET);
    std::vector<std::uint8_t> bytes(ok ? static_cast<std::size_t>(length) : 0);
    ok = ok && std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fgetc(file) == EOF && !std::ferror(file);
    const bool close_error = std::fclose(file) != 0;
    if (!ok || close_error || !variants.Parse(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "invalid PBR variants\n"); return false;
    }
    return true;
}

bool ReadCameraUpdate(const wchar_t* path, Variants& variants) {
    FILE* file = _wfopen(path, L"rb");
    if (!file) { std::fprintf(stderr, "cannot open camera update packet\n"); return false; }
    std::vector<std::uint8_t> bytes(t3render::scene::kMaxPacketBytes + 1);
    const std::size_t size = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool error = std::ferror(file) != 0;
    const bool close_error = std::fclose(file) != 0;
    if (error || close_error || !variants.UpdateCamera(bytes.data(), size)) {
        std::fprintf(stderr, "camera update changes resident scene or has invalid camera\n"); return false;
    }
    return true;
}

struct Constants {
    float view[16], projection[16];
    float size_scale_count[4], camera_exposure[4], ambient[4];
    float light_position_radius[kMaxLights][4], light_color_intensity[kMaxLights][4];
    float shadow_settings[4];
    float jitter[4];
};
static_assert(sizeof(Constants) == 720, "unexpected PBR constant-buffer layout");
struct PreviousConstants {
    float view[16], projection[16], camera_scale[4], jitter_weight[4];
};
static_assert(sizeof(PreviousConstants) == 160, "unexpected temporal constant-buffer layout");

bool WriteTemporal(const wchar_t* path, const Frame& frame, const std::uint8_t* digest,
                   float jitter_x, float jitter_y, std::uint32_t flags,
                   const std::uint8_t* color, const std::uint8_t* position,
                   const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
    Handle file{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.p == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "cannot create temporal buffers: %lu\n", GetLastError()); return false; }
    const char magic[8] = {'T','3','T','E','M','P','0','1'};
    const float jitter[2] = {jitter_x, jitter_y};
    if (!WriteAll(file.p, magic, sizeof(magic)) || !WriteAll(file.p, &frame.width, sizeof(frame.width)) ||
        !WriteAll(file.p, &frame.height, sizeof(frame.height)) || !WriteAll(file.p, digest, 32) ||
        !WriteAll(file.p, frame.view, sizeof(frame.view)) || !WriteAll(file.p, frame.projection, sizeof(frame.projection)) ||
        !WriteAll(file.p, frame.camera, sizeof(frame.camera)) || !WriteAll(file.p, &frame.meters_per_unit, sizeof(float)) ||
        !WriteAll(file.p, &frame.exposure, sizeof(float)) || !WriteAll(file.p, jitter, sizeof(jitter)) ||
        !WriteAll(file.p, &flags, sizeof(flags))) return false;
    for (const auto* bytes : {color, position}) for (UINT y = 0; y < frame.height; ++y) {
        const auto* row = bytes+fp.Offset+UINT64(y)*fp.Footprint.RowPitch;
        if (!WriteAll(file.p, row, frame.width*8)) return false;
    }
    return true;
}

bool UploadHistoryImage(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT width, UINT height,
                        const std::vector<std::uint8_t>& bytes, GpuTexture& gpu) {
    if (bytes.size() != std::size_t(width)*height*8) return false;
    const auto desc = Image(width, height, 1, kHdr, D3D12_RESOURCE_FLAG_NONE);
    const auto default_heap = Heap(D3D12_HEAP_TYPE_DEFAULT), upload_heap = Heap(D3D12_HEAP_TYPE_UPLOAD);
    if (!Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
               nullptr, IID_PPV_ARGS(gpu.texture.put())), "CreateHistoryTexture")) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 row_size = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (rows != height || row_size != UINT64(width)*8 || fp.Footprint.RowPitch < row_size || !total || total > SIZE_MAX ||
        fp.Offset > total || row_size > total-fp.Offset || UINT64(height-1) > (total-fp.Offset-row_size)/fp.Footprint.RowPitch) return false;
    const auto upload_desc = Buffer(total);
    if (!Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(gpu.upload.put())), "CreateHistoryUpload")) return false;
    void* mapped = nullptr; const D3D12_RANGE no_read{0, 0};
    if (!Check(gpu.upload->Map(0, &no_read, &mapped), "MapHistoryUpload") || !mapped) return false;
    for (UINT y = 0; y < height; ++y)
        std::memcpy(static_cast<std::uint8_t*>(mapped)+fp.Offset+UINT64(y)*fp.Footprint.RowPitch,
                    bytes.data()+std::size_t(y)*width*8, std::size_t(width)*8);
    const D3D12_RANGE written{0, static_cast<SIZE_T>(total)}; gpu.upload->Unmap(0, &written);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = gpu.texture.p; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = gpu.upload.p; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, gpu.texture.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

struct VariantRun {
    Variants* variants = nullptr;
    std::string key;
    const wchar_t* replacement_output = nullptr;
    const wchar_t* restored_output = nullptr;
    bool stream = false;
    std::vector<std::uint8_t> draws, casters;
};

int Render(const Frame& frame, const wchar_t* output, bool indirect, bool gtao, bool volumetric, bool reflections, bool filmic,
           const wchar_t* temporal_output, float jitter_x, float jitter_y, const std::uint8_t* digest,
           const t3render::temporal::History* history, float history_weight, VariantRun* variant = nullptr) {
    if (variant && (!variant->variants || temporal_output || history || variant->draws.size() != frame.draws.size() ||
                    variant->casters.size() != frame.offline_casters.size())) return 2;
    const bool use_gbuffer = indirect || gtao || volumetric || reflections || temporal_output;
    ComPtr<ID3D12Device> device;
    if (!SelectDevice(device)) return 1;
    Module module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!module.p) { std::fprintf(stderr, "d3dcompiler_47.dll unavailable\n"); return 1; }
    FARPROC address = GetProcAddress(module.p, "D3DCompile");
    CompileFn compile = nullptr;
    static_assert(sizeof(compile) == sizeof(address), "unexpected function pointer size");
    std::memcpy(&compile, &address, sizeof(compile));
    if (!compile) return 1;
    static const char vs_source[] = R"(
cbuffer Scene : register(b0) {
    row_major float4x4 view; row_major float4x4 projection;
    float4 size_scale_count; float4 camera_exposure; float4 ambient;
    float4 light_position_radius[16]; float4 light_color_intensity[16];
    float4 shadow_settings; float4 jitter;
};
struct O { float4 uv:TEXCOORD0; float3 world:TEXCOORD1; float3 normal:TEXCOORD2;
           float4 tangent:TEXCOORD3; float4 position:SV_Position; };
O main(float3 p:POSITION, float2 uv:TEXCOORD0, float3 n:NORMAL, float4 t:TANGENT) {
    O o; o.position=mul(mul(float4(p,1),view),projection);
    o.position.x+=o.position.w*(1+2*jitter.x)/size_scale_count.x;
    o.position.y-=o.position.w*(1+2*jitter.y)/size_scale_count.y;
    o.uv=float4(uv,0,0); o.world=p; o.normal=n; o.tangent=t; return o;
})";
    static const char ps_source[] = R"(
Texture2D albedo_tex:register(t0); Texture2D normal_tex:register(t1);
Texture2D roughness_tex:register(t2); Texture2D ao_tex:register(t3); SamplerState samp:register(s0);
#if T3_SHADOWS
TextureCubeArray shadow_maps:register(t5); SamplerState shadow_sampler:register(s1);
#endif
cbuffer Scene : register(b0) {
    row_major float4x4 view; row_major float4x4 projection;
    float4 size_scale_count; float4 camera_exposure; float4 ambient;
    float4 light_position_radius[16]; float4 light_color_intensity[16];
    float4 shadow_settings; float4 jitter;
};
#if T3_EMISSION || T3_METALLIC
cbuffer Material : register(b2) {
    float3 emissive_radiance;
#if T3_METALLIC
    float metallic;
#endif
};
#endif
#if T3_SHADOWS
float visibility(float3 world, float3 geometric_normal, float3 to_light, uint light_index) {
    float3 ray=world-light_position_radius[light_index].xyz;
    float ray_length=length(ray), distance=ray_length*size_scale_count.z;
    if(distance>=shadow_settings.x || ray_length<0.000001) return 1;
    float3 direction=ray/ray_length;
    float3 axis=abs(direction.z)<0.95?float3(0,0,1):float3(0,1,0);
    float3 tangent=normalize(cross(axis,direction)); float3 bitangent=cross(direction,tangent);
    float radius=min(0.15,light_position_radius[light_index].w/max(distance,0.01)*shadow_settings.z);
    if(radius>0) radius=max(radius,1/shadow_settings.w);
    float slope=1+2*(1-saturate(dot(geometric_normal,to_light)));
    float plane_numerator=dot(geometric_normal,direction);
    float plane_cos=abs(plane_numerator);
    // Point sampling returns a texel-center depth rather than the exact ray.
    // Bound its radial error using the receiver slope and cubemap resolution.
    float texel_slope=min(4,sqrt(saturate(1-plane_cos*plane_cos))/max(plane_cos,0.1));
    float texel_bias=distance*(2/shadow_settings.w)*texel_slope;
    float visible=0;
    [unroll] for(int y=-2;y<=2;++y) [unroll] for(int x=-2;x<=2;++x) {
        float3 sample_dir=normalize(direction+(tangent*x+bitangent*y)*(radius*0.5));
        // Compare against the receiver's tangent plane along each sample ray;
        // a constant radial distance creates false self-shadow on sloped planes.
        float plane_denominator=dot(geometric_normal,sample_dir);
        float plane_scale=(abs(plane_denominator)>0.05 && plane_numerator*plane_denominator>0)
                          ? clamp(plane_numerator/plane_denominator,0.5,2) : 1;
        float receiver=(distance*plane_scale-shadow_settings.y*slope-texel_bias)/shadow_settings.x;
        float depth=shadow_maps.SampleLevel(shadow_sampler,float4(sample_dir,(float)light_index),0).r;
        visible+=receiver<=depth?1:0;
    }
    return visible/25;
}
#endif
#if T3_GBUFFER
struct Output { float4 color:SV_Target0; float4 position:SV_Target1; float4 normal:SV_Target2;
                float4 diffuse:SV_Target3; float4 albedo:SV_Target4; };
Output main(float4 uv:TEXCOORD0, float3 world:TEXCOORD1, float3 normal:TEXCOORD2, float4 tangent:TEXCOORD3) {
#else
float4 main(float4 uv:TEXCOORD0, float3 world:TEXCOORD1, float3 normal:TEXCOORD2,
            float4 tangent:TEXCOORD3):SV_Target {
#endif
    const float pi=3.14159265359;
    float3 albedo=albedo_tex.Sample(samp,uv.xy).rgb; // sRGB SRV filters in linear light.
    float roughness=clamp(roughness_tex.Sample(samp,uv.xy).r,0.04,1);
    float ao=saturate(ao_tex.Sample(samp,uv.xy).r);
    float3 n=normal*rsqrt(max(dot(normal,normal),0.00000001));
    float3 geometric_normal=n;
    if (abs(tangent.w)>0.5) {
        float3 t=tangent.xyz-n*dot(n,tangent.xyz); float t2=dot(t,t);
        float3 mapped=normal_tex.Sample(samp,uv.xy).rgb*2-1; float m2=dot(mapped,mapped);
        if(t2>0.00000001&&m2>0.00000001) {
            t*=rsqrt(t2); mapped*=rsqrt(m2);
            float3 mapped_world=t*mapped.x+cross(n,t)*tangent.w*mapped.y+n*mapped.z;
            n=mapped_world*rsqrt(max(dot(mapped_world,mapped_world),0.00000001));
        }
    }
    float3 view_delta=camera_exposure.xyz-world;
    float3 v=view_delta*rsqrt(max(dot(view_delta,view_delta),0.00000001));
    float nv=saturate(dot(n,v));
#if T3_METALLIC
    float3 result=albedo*(1-metallic)*ambient.rgb*ao;
#else
    float3 result=albedo*ambient.rgb*ao;
#endif
#if T3_GBUFFER
    float3 direct_diffuse=0;
#endif
#if T3_EMISSION
    result+=emissive_radiance;
#if T3_GBUFFER
    direct_diffuse+=emissive_radiance;
#endif
#endif
    float alpha=roughness*roughness; float a2=alpha*alpha; float k=(roughness+1)*(roughness+1)/8;
    [loop] for(uint i=0;i<(uint)size_scale_count.w;i++) {
        float3 to_light=(light_position_radius[i].xyz-world)*size_scale_count.z;
        float d2=dot(to_light,to_light); float3 l=to_light*rsqrt(max(d2,0.00000001));
        float nl=saturate(dot(n,l));
        if(nl>0&&nv>0) {
            float3 half_sum=v+l; float3 h=half_sum*rsqrt(max(dot(half_sum,half_sum),0.00000001));
            float nh=saturate(dot(n,h)), vh=saturate(dot(v,h));
            float denom=nh*nh*(a2-1)+1;
            float distribution=a2/max(pi*denom*denom,0.00000001);
            float geometry=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));
#if T3_METALLIC
            float3 f0=lerp(float3(0.04,0.04,0.04),albedo,metallic);
            float3 fresnel=f0+(1-f0)*pow(1-vh,5);
#else
            float3 fresnel=0.04+(1-0.04)*pow(1-vh,5);
#endif
            float3 specular=distribution*geometry*fresnel/max(4*nv*nl,0.00000001);
#if T3_METALLIC
            float3 diffuse=(1-fresnel)*(1-metallic)*albedo/pi;
#else
            float3 diffuse=(1-fresnel)*albedo/pi;
#endif
            float radius=light_position_radius[i].w;
            float3 radiance=light_color_intensity[i].rgb*light_color_intensity[i].w/max(d2,radius*radius);
            float light_visibility=1;
#if T3_SHADOWS
            light_visibility=visibility(world,geometric_normal,l,i);
#endif
            result+=(diffuse+specular)*radiance*nl*light_visibility;
#if T3_GBUFFER
            direct_diffuse+=diffuse*radiance*nl*light_visibility;
#endif
        }
    }
    // Avoid half-float overflow producing inf/NaN in the display pass.
#if T3_GBUFFER
    Output o; o.color=float4(min(max(result,0),65504),1);
    // Camera-relative metres keep half-float precision independent of map origin.
    float3 relative=(world-camera_exposure.xyz)*size_scale_count.z;
    o.position=all(abs(relative)<=60000)?float4(relative,1):float4(0,0,0,0);
    o.normal=float4(geometric_normal,roughness);
#if T3_METALLIC
    o.diffuse=float4(min(max(direct_diffuse,0),65504),metallic);
#else
    o.diffuse=float4(min(max(direct_diffuse,0),65504),1);
#endif
    o.albedo=float4(albedo,ao); return o;
#else
    return float4(min(max(result,0),65504),1);
#endif
})";
    static const char shadow_vs_source[] = R"(
cbuffer ShadowPass:register(b1) {
    float4 light_scale; float4 face_right; float4 face_up; float4 face_forward; float4 clip_range;
};
struct O {float3 world:TEXCOORD0; float4 position:SV_Position;};
O main(float3 position:POSITION) {
    O o; float3 p=(position-light_scale.xyz)*light_scale.w;
    float z=dot(p,face_forward.xyz);
    o.position=float4(dot(p,face_right.xyz),dot(p,face_up.xyz),
                      (z*clip_range.y-clip_range.x*clip_range.y)/(clip_range.y-clip_range.x),z);
    o.world=position; return o;
})";
    static const char shadow_ps_source[] = R"(
cbuffer ShadowPass:register(b1) {
    float4 light_scale; float4 face_right; float4 face_up; float4 face_forward; float4 clip_range;
};
float main(float3 world:TEXCOORD0):SV_Target {
    return saturate(length((world-light_scale.xyz)*light_scale.w)/clip_range.y);
})";
    static const char tone_vs_source[] = R"(
float4 main(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1);
})";
    static const char tone_ps_source[] = R"(
Texture2D hdr:register(t4);
#if T3_GBUFFER
Texture2D positions:register(t6); Texture2D normals:register(t7);
Texture2D diffuse_light:register(t8); Texture2D surface_albedo:register(t9);
#endif
#if T3_VOLUMETRIC && T3_SHADOWS
TextureCubeArray volume_shadows:register(t5); SamplerState volume_shadow_sampler:register(s1);
#endif
cbuffer Scene : register(b0) {
    row_major float4x4 view; row_major float4x4 projection;
    float4 size_scale_count; float4 camera_exposure; float4 ambient;
    float4 light_position_radius[16]; float4 light_color_intensity[16];
    float4 shadow_settings; float4 jitter;
};
#if T3_INDIRECT
float3 indirect_light(int2 pixel) {
    const float pi=3.14159265359;
    int2 size=int2(size_scale_count.xy);
    float4 p=positions.Load(int3(pixel,0));
    if(p.w<0.5) return 0;
    float3 n=normals.Load(int3(pixel,0)).xyz;
    float4 receiver=surface_albedo.Load(int3(pixel,0));
    float radius=clamp(min(size.x,size.y)*0.125,8,64);
    float3 sum=0;
    [unroll] for(int i=0;i<24;++i) {
        float r=sqrt((i+0.5)/24.0)*radius;
        float angle=i*2.39996323;
        int2 qpixel=pixel+int2(round(float2(cos(angle),sin(angle))*r));
        if(any(qpixel<0)||any(qpixel>=size-1)) continue;
        float4 q=positions.Load(int3(qpixel,0));
        float4 qx=positions.Load(int3(qpixel+int2(1,0),0));
        float4 qy=positions.Load(int3(qpixel+int2(0,1),0));
        if(q.w<0.5||qx.w<0.5||qy.w<0.5) continue;
        float3 dx=qx.xyz-q.xyz, dy=qy.xyz-q.xyz;
        if(dot(dx,dx)>0.25||dot(dy,dy)>0.25) continue;
        float3 delta=q.xyz-p.xyz; float d2=dot(delta,delta);
        if(d2<0.0025||d2>9) continue;
        float3 direction=delta*rsqrt(d2);
        float3 qn=normals.Load(int3(qpixel,0)).xyz;
        if(dot(qn,normals.Load(int3(qpixel+int2(1,0),0)).xyz)<0.9||
           dot(qn,normals.Load(int3(qpixel+int2(0,1),0)).xyz)<0.9) continue;
        float receiver_cos=saturate(dot(n,direction));
        float source_cos=saturate(dot(qn,-direction));
        if(receiver_cos<=0||source_cos<=0) continue;
        // Reject sampled screen-space blockers. This cannot detect geometry
        // outside the image or reconstruct visibility behind the first layer.
        float zp=mul(float4(p.xyz,0),view).z, zq=mul(float4(q.xyz,0),view).z;
        bool blocked=false;
        if(zp>0.001&&zq>0.001) {
            [unroll] for(int j=1;j<=2;++j) {
                float t=j/3.0;
                int2 middle=int2(round(lerp(float2(pixel),float2(qpixel),t)));
                float4 hit=positions.Load(int3(middle,0));
                float expected=abs(projection[2][3])>0.00001 ? 1/lerp(1/zp,1/zq,t) : lerp(zp,zq,t);
                float actual=mul(float4(hit.xyz,0),view).z;
                if(hit.w>0.5&&actual>0&&actual+0.025<expected) blocked=true;
            }
        }
        if(blocked) continue;
        float area=length(cross(dx,dy))*pi*radius*radius/24;
        float form=min(area/d2,1)*receiver_cos*source_cos;
        sum+=diffuse_light.Load(int3(qpixel,0)).rgb*form;
    }
#if T3_METALLIC
    return min(sum*receiver.rgb*receiver.a*(1-diffuse_light.Load(int3(pixel,0)).a)/pi,65504);
#else
    return min(sum*receiver.rgb*receiver.a/pi,65504);
#endif
}
#endif
#if T3_GTAO
float slice_integral(float nv, float nt, float angle) {
    float s=sin(angle);
    return max(0,0.5*nv*s*s+nt*(0.5*angle-0.25*sin(2*angle)));
}
float ambient_visibility(int2 pixel) {
    const float half_pi=1.57079632679;
    int2 size=int2(size_scale_count.xy);
    float4 p=positions.Load(int3(pixel,0));
    float distance2=dot(p.xyz,p.xyz);
    if(p.w<0.5||distance2<0.000001) return 1;
    float3 v=-p.xyz*rsqrt(distance2);
    float3 n=normals.Load(int3(pixel,0)).xyz;
    float nv=max(dot(n,v),0);
    float3 camera_right=float3(view[0][0],view[1][0],view[2][0]);
    float3 camera_up=float3(view[0][1],view[1][1],view[2][1]);
    float radius=clamp(min(size.x,size.y)*0.0625,8,32);
    float visible=0, full=0;
    [unroll] for(int slice=0;slice<4;++slice) {
        float phi=slice*0.78539816339;
        float2 direction=float2(cos(phi),sin(phi));
        float3 axis=camera_right*direction.x-camera_up*direction.y;
        axis-=v*dot(axis,v);
        float axis2=dot(axis,axis);
        if(axis2<0.000001) continue;
        axis*=rsqrt(axis2);
        [unroll] for(int side=-1;side<=1;side+=2) {
            float3 tangent=axis*side;
            float nt=dot(n,tangent);
            float limit=nt<0?atan2(nv,-nt):half_pi;
            float horizon=half_pi;
            [unroll] for(int step=1;step<=6;++step) {
                float offset=radius*step/6;
                int2 qpixel=pixel+int2(round(direction*(side*offset)));
                if(any(qpixel<0)||any(qpixel>=size)||all(qpixel==pixel)) continue;
                float4 q=positions.Load(int3(qpixel,0));
                if(q.w<0.5) continue;
                float3 delta=q.xyz-p.xyz;
                float d2=dot(delta,delta), x=dot(delta,tangent), z=dot(delta,v);
                if(d2<0.0004||d2>=1||x<=0||z<=0.025||dot(delta,n)<=0.025) continue;
                float angle=atan2(x,z-0.025);
                horizon=min(horizon,lerp(half_pi,angle,saturate(1-sqrt(d2))));
            }
            full+=slice_integral(nv,nt,limit);
            visible+=slice_integral(nv,nt,min(limit,horizon));
        }
    }
    return full>0.00001?saturate(visible/full):1;
}
#endif
#if T3_REFLECTIONS
float3 screen_reflection(int2 pixel) {
    int2 size=int2(size_scale_count.xy);
    float4 p=positions.Load(int3(pixel,0));
    float4 nr=normals.Load(int3(pixel,0));
    float p2=dot(p.xyz,p.xyz);
    if(p.w<0.5||p2<0.000001||nr.w>=0.95) return 0;
    float3 v=-p.xyz*rsqrt(p2);
    float nv=saturate(dot(nr.xyz,v));
    if(nv<=0.0001) return 0;
    float3 ray=normalize(reflect(-v,nr.xyz));
    float3 origin=p.xyz+nr.xyz*0.03;
    [loop] for(int step=1;step<=48;++step) {
        float distance=0.1+(step-1)*0.15;
        float3 sample_position=origin+ray*distance;
        float3 world=camera_exposure.xyz+sample_position/size_scale_count.z;
        float4 clip=mul(mul(float4(world,1),view),projection);
        if(clip.w<=0.00001) break;
        clip.x+=clip.w*(1+2*jitter.x)/size_scale_count.x;
        clip.y-=clip.w*(1+2*jitter.y)/size_scale_count.y;
        float3 ndc=clip.xyz/clip.w;
        if(any(abs(ndc.xy)>1)||ndc.z<0||ndc.z>1) break;
        int2 qpixel=int2((ndc.x*0.5+0.5)*size.x,(0.5-ndc.y*0.5)*size.y);
        if(any(qpixel<0)||any(qpixel>=size)) break;
        float4 hit=positions.Load(int3(qpixel,0));
        if(hit.w<0.5||dot(hit.xyz-p.xyz,hit.xyz-p.xyz)<0.01) continue;
        float ray_depth=mul(float4(sample_position,0),view).z;
        float hit_depth=mul(float4(hit.xyz,0),view).z;
        float thickness=0.08+distance*0.025;
        if(ray_depth<hit_depth||ray_depth-hit_depth>thickness) continue;
        // Small roughness-dependent filter; reject samples from different depths.
        int radius=(int)round(nr.w*4);
        float3 reflected=0; float weight=0;
        [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
            int2 q=qpixel+int2(x,y)*radius;
            if(any(q<0)||any(q>=size)) continue;
            float4 neighbor=positions.Load(int3(q,0));
            if(neighbor.w<0.5||abs(mul(float4(neighbor.xyz-hit.xyz,0),view).z)>thickness) continue;
            reflected+=hdr.Load(int3(q,0)).rgb; weight+=1;
        }
        if(weight==0) return 0;
#if T3_METALLIC
        float3 base_color=surface_albedo.Load(int3(pixel,0)).rgb;
        float metal=diffuse_light.Load(int3(pixel,0)).a;
        float3 f0=lerp(float3(0.04,0.04,0.04),base_color,metal);
        float3 fresnel=f0+(1-f0)*pow(1-nv,5);
#else
        float fresnel=0.04+0.96*pow(1-nv,5);
#endif
        float roughness_fade=(1-nr.w)*(1-nr.w);
        float edge=saturate((1-max(abs(ndc.x),abs(ndc.y)))*8);
        return reflected/weight*fresnel*roughness_fade*edge*saturate((7.3-distance)/1.5);
    }
    return 0;
}
#endif
#if T3_VOLUMETRIC
float3 volume_lighting(float3 ray, float distance) {
    const float extinction=0.06, scattering=0.045, anisotropy=0.2;
    const float inv4pi=0.07957747155;
    float travel=min(distance,50), ds=travel/16;
    float3 direction=ray/max(distance,0.00001), sum=0;
    [loop] for(int sample_index=0;sample_index<16;++sample_index) {
        float along=(sample_index+0.5)*ds;
        float3 sample_position=direction*along;
        float transmittance=exp(-extinction*along);
        [loop] for(uint i=0;i<(uint)size_scale_count.w;++i) {
            float3 light=(light_position_radius[i].xyz-camera_exposure.xyz)*size_scale_count.z-sample_position;
            float d2=dot(light,light), d=sqrt(d2);
            if(d<0.00001) continue;
            float3 to_light=light/d;
            float cosine=clamp(dot(to_light,direction),-1,1);
            float phase=(1-anisotropy*anisotropy)*inv4pi/
                        pow(max(1+anisotropy*anisotropy-2*anisotropy*cosine,0.01),1.5);
            float visible=1;
#if T3_SHADOWS
            if(d<shadow_settings.x) {
                float depth=volume_shadows.SampleLevel(volume_shadow_sampler,float4(-to_light,(float)i),0).r;
                visible=(d-shadow_settings.y)/shadow_settings.x<=depth?1:0;
            }
#endif
            float radius=light_position_radius[i].w;
            float3 intensity=light_color_intensity[i].rgb*light_color_intensity[i].w;
            sum+=intensity/max(d2,radius*radius)*exp(-extinction*d)*visible*phase*transmittance*scattering*ds;
        }
    }
    return min(max(sum,0),65504);
}
#endif
#if T3_TEMPORAL_EXPORT
struct ToneOutput { float4 display:SV_Target0; float4 hdr_color:SV_Target1; };
ToneOutput main(float4 position:SV_Position) {
#else
float4 main(float4 position:SV_Position):SV_Target {
#endif
    float3 c=hdr.Load(int3(position.xy,0)).rgb;
#if T3_GTAO
    float visibility=ambient_visibility(int2(position.xy));
    float4 surface=surface_albedo.Load(int3(position.xy,0));
#if T3_METALLIC
    c=max(c-surface.rgb*surface.a*(1-diffuse_light.Load(int3(int2(position.xy),0)).a)*ambient.rgb*(1-visibility),0);
#else
    c=max(c-surface.rgb*surface.a*ambient.rgb*(1-visibility),0);
#endif
#endif
#if T3_INDIRECT
    float3 bounce=indirect_light(int2(position.xy));
#if T3_GTAO
    bounce*=visibility;
#endif
    c+=bounce;
#endif
#if T3_REFLECTIONS
    c+=screen_reflection(int2(position.xy));
#endif
#if T3_VOLUMETRIC
    float4 endpoint=positions.Load(int3(position.xy,0));
    if(endpoint.w>0.5) {
        float distance=length(endpoint.xyz);
        if(distance>0.00001) c=c*exp(-0.06*distance)+volume_lighting(endpoint.xyz,distance);
    }
#endif
#if T3_TEMPORAL_EXPORT
    float3 linear_result=min(max(c,0),65504);
#endif
    c=max(c*camera_exposure.w,0);
#if T3_FILMIC
    c=(c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14);
#else
    c=c/(1+c);
#endif
    c=lerp(c*12.92,1.055*pow(c,1/2.4)-0.055,step(0.0031308,c));
#if T3_TEMPORAL_EXPORT
    ToneOutput o; o.display=float4(saturate(c),1); o.hdr_color=float4(linear_result,1); return o;
#else
    return float4(saturate(c),1);
#endif
})";
    static const char temporal_ps_source[] = R"(
Texture2D current_color:register(t12); Texture2D current_position:register(t6);
Texture2D history_color:register(t10); Texture2D history_position:register(t11);
cbuffer Scene : register(b0) {
    row_major float4x4 view; row_major float4x4 projection;
    float4 size_scale_count; float4 camera_exposure; float4 ambient;
    float4 light_position_radius[16]; float4 light_color_intensity[16];
    float4 shadow_settings; float4 jitter;
};
cbuffer Previous : register(b3) {
    row_major float4x4 previous_view; row_major float4x4 previous_projection;
    float4 previous_camera_scale; float4 previous_jitter_weight;
};
struct Output { float4 display:SV_Target0; float4 hdr_color:SV_Target1; };
Output main(float4 position:SV_Position) {
    int2 size=int2(size_scale_count.xy), pixel=int2(position.xy);
    float3 color=current_color.Load(int3(pixel,0)).rgb;
    float4 p=current_position.Load(int3(pixel,0));
    if(p.w>0.5) {
        float3 world=camera_exposure.xyz+p.xyz/size_scale_count.z;
        float4 clip=mul(mul(float4(world,1),previous_view),previous_projection);
        if(clip.w>0.00000001&&clip.z>=0&&clip.z<=clip.w) {
            float2 screen=float2((clip.x/clip.w*0.5+0.5)*size.x+0.5+previous_jitter_weight.x,
                                 (0.5-clip.y/clip.w*0.5)*size.y+0.5+previous_jitter_weight.y);
            if(all(screen>=0)&&all(screen<float2(size))) {
                int2 base=int2(screen);
                float2 fraction=screen-float2(base);
                float2 blend=saturate((fraction-(1.0-2.0/32.0))*32.0);
                float3 expected=p.xyz+(camera_exposure.xyz-previous_camera_scale.xyz)*size_scale_count.z;
                float3 history=0; float total=0;
                [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
                    int2 q=min(base+int2(x,y),size-1);
                    float w=(x?blend.x:1-blend.x)*(y?blend.y:1-blend.y);
                    if(w>0) {
                        float4 old_position=history_position.Load(int3(q,0));
                        float3 difference=old_position.xyz-expected;
                        if(old_position.w>0.5&&dot(difference,difference)<=0.0025) {
                            history+=history_color.Load(int3(q,0)).rgb*w;
                            total+=w;
                        }
                    }
                }
                if(total>0.000001) {
                    float3 low=color, high=color;
                    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
                        int2 nearby=clamp(pixel+int2(x,y),int2(0,0),size-1);
                        float3 sample=current_color.Load(int3(nearby,0)).rgb;
                        low=min(low,sample); high=max(high,sample);
                    }
                    float3 old=clamp(history/total,low,high);
                    color=lerp(color,old,previous_jitter_weight.z);
                }
            }
        }
    }
    color=min(max(color,0),65504);
    float3 mapped=max(color*camera_exposure.w,0);
#if T3_FILMIC
    mapped=(mapped*(2.51*mapped+0.03))/(mapped*(2.43*mapped+0.59)+0.14);
#else
    mapped=mapped/(1+mapped);
#endif
    mapped=lerp(mapped*12.92,1.055*pow(mapped,1/2.4)-0.055,step(0.0031308,mapped));
    Output o; o.display=float4(saturate(mapped),1); o.hdr_color=float4(color,1); return o;
})";
    ComPtr<ID3DBlob> vs, ps, tone_vs, tone_ps, shadow_vs, shadow_ps, temporal_ps;
    const std::string options = std::string(use_gbuffer ? "#define T3_GBUFFER 1\n" : "#define T3_GBUFFER 0\n") +
                                (indirect ? "#define T3_INDIRECT 1\n" : "#define T3_INDIRECT 0\n") +
                                (gtao ? "#define T3_GTAO 1\n" : "#define T3_GTAO 0\n") +
                                (volumetric ? "#define T3_VOLUMETRIC 1\n" : "#define T3_VOLUMETRIC 0\n") +
                                (reflections ? "#define T3_REFLECTIONS 1\n" : "#define T3_REFLECTIONS 0\n") +
                                (filmic ? "#define T3_FILMIC 1\n" : "#define T3_FILMIC 0\n") +
                                (temporal_output ? "#define T3_TEMPORAL_EXPORT 1\n" : "#define T3_TEMPORAL_EXPORT 0\n") +
                                (frame.has_emission ? "#define T3_EMISSION 1\n" : "#define T3_EMISSION 0\n") +
                                (frame.has_metallic ? "#define T3_METALLIC 1\n" : "#define T3_METALLIC 0\n");
    const std::string shadow_option = frame.shadow_size ? "#define T3_SHADOWS 1\n" : "#define T3_SHADOWS 0\n";
    const std::string pixel_source = shadow_option + options + ps_source;
    const std::string tone_source = shadow_option + options + tone_ps_source;
    const std::string temporal_source = options + temporal_ps_source;
    if (!Compile(compile, vs_source, "vs_5_0", vs) || !Compile(compile, pixel_source.c_str(), "ps_5_0", ps) ||
        (frame.shadow_size && (!Compile(compile, shadow_vs_source, "vs_5_0", shadow_vs) ||
                               !Compile(compile, shadow_ps_source, "ps_5_0", shadow_ps))) ||
        !Compile(compile, tone_vs_source, "vs_5_0", tone_vs) || !Compile(compile, tone_source.c_str(), "ps_5_0", tone_ps) ||
        (history && !Compile(compile, temporal_source.c_str(), "ps_5_0", temporal_ps))) return 1;

    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12Fence> fence;
    if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateQueue") ||
        !Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateAllocator") ||
        !Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence")) return 1;
    Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!event.p) return 1;
    D3D12_DESCRIPTOR_RANGE ranges[13]{};
    D3D12_ROOT_PARAMETER params[17]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 6; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        params[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i+1].DescriptorTable.NumDescriptorRanges = 1; params[i+1].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i+1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[7].Constants.ShaderRegister = 1; params[7].Constants.Num32BitValues = 20;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 6; i < 10; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        params[i+2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i+2].DescriptorTable.NumDescriptorRanges = 1; params[i+2].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i+2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[12].Constants.ShaderRegister = 2; params[12].Constants.Num32BitValues = frame.has_metallic ? 4 : 3;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[13].Descriptor.ShaderRegister = 3;
    params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT i = 10; i < 13; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        params[i+4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i+4].DescriptorTable.NumDescriptorRanges = 1; params[i+4].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i+4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    auto& sampler = samplers[0];
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    auto& shadow_sampler = samplers[1]; shadow_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    shadow_sampler.AddressU = shadow_sampler.AddressV = shadow_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadow_sampler.MaxAnisotropy = 1; shadow_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    shadow_sampler.MaxLOD = D3D12_FLOAT32_MAX; shadow_sampler.ShaderRegister = 1;
    shadow_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{}; root_desc.NumParameters = history ? 17 : frame.has_emission ? 13 : use_gbuffer ? 12 : 8; root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 2; root_desc.pStaticSamplers = samplers;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, root_errors; ComPtr<ID3D12RootSignature> root;
    if (!Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, root_blob.put(), root_errors.put()), "SerializeRoot") ||
        !Check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(root.put())), "CreateRoot")) return 1;
    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = root.p;
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.InputLayout = {input, 4}; desc.SampleMask = UINT_MAX;
    for (auto& blend : desc.BlendState.RenderTarget) {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = use_gbuffer ? 5 : 1;
    for (UINT i = 0; i < desc.NumRenderTargets; ++i) desc.RTVFormats[i] = kHdr;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT; desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline, tone_pipeline, shadow_pipeline, temporal_pipeline;
    if (!Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline.put())), "CreatePbrPipeline")) return 1;
    if (frame.shadow_size) {
        auto shadow_desc = desc;
        shadow_desc.VS = {shadow_vs->GetBufferPointer(), shadow_vs->GetBufferSize()};
        shadow_desc.PS = {shadow_ps->GetBufferPointer(), shadow_ps->GetBufferSize()};
        shadow_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        shadow_desc.NumRenderTargets = 1;
        for (UINT i = 1; i < 5; ++i) shadow_desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        shadow_desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        if (!Check(device->CreateGraphicsPipelineState(&shadow_desc, IID_PPV_ARGS(shadow_pipeline.put())), "CreateShadowPipeline")) return 1;
    }
    desc.VS = {tone_vs->GetBufferPointer(), tone_vs->GetBufferSize()}; desc.PS = {tone_ps->GetBufferPointer(), tone_ps->GetBufferSize()};
    desc.InputLayout = {nullptr, 0}; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.DepthStencilState.DepthEnable = FALSE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.NumRenderTargets = temporal_output ? 2 : 1;
    for (UINT i = 1; i < 5; ++i) desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    if (temporal_output) desc.RTVFormats[1] = kHdr;
    desc.RTVFormats[0] = kColor; desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    if (!Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(tone_pipeline.put())), "CreateTonePipeline")) return 1;
    if (history) {
        desc.PS = {temporal_ps->GetBufferPointer(), temporal_ps->GetBufferSize()};
        if (!Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(temporal_pipeline.put())), "CreateTemporalPipeline")) return 1;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pipeline.p,
                                         IID_PPV_ARGS(list.put())), "CreateList")) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = static_cast<UINT>(frame.textures.size()*2+1+(frame.shadow_size ? 1 : 0)+(use_gbuffer ? 4 : 0)+(history ? 3 : 0));
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    if (!Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srv_heap.put())), "CreateSrvHeap")) return 1;
    auto cpu_srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<std::unique_ptr<GpuTexture>> textures;
    for (const auto& texture : frame.textures) {
        auto gpu = std::make_unique<GpuTexture>();
        if (!UploadTexture(device.p, list.p, texture, *gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS)) return 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = kColor; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = static_cast<UINT>(texture.mips.size());
        device->CreateShaderResourceView(gpu->texture.p, &srv, cpu_srv); cpu_srv.ptr += srv_step;
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        device->CreateShaderResourceView(gpu->texture.p, &srv, cpu_srv); cpu_srv.ptr += srv_step;
        textures.push_back(std::move(gpu));
    }
    const auto default_heap = Heap(D3D12_HEAP_TYPE_DEFAULT), upload_heap = Heap(D3D12_HEAP_TYPE_UPLOAD), read_heap = Heap(D3D12_HEAP_TYPE_READBACK);
    const auto hdr_desc = Image(frame.width, frame.height, 1, kHdr, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto output_desc = Image(frame.width, frame.height, 1, kColor, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto depth_desc = Image(frame.width, frame.height, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{}; clear.Format = kHdr;
    D3D12_CLEAR_VALUE output_clear{}; output_clear.Format = kColor;
    D3D12_CLEAR_VALUE depth_clear{}; depth_clear.Format = DXGI_FORMAT_D32_FLOAT; depth_clear.DepthStencil.Depth = 1;
    ComPtr<ID3D12Resource> hdr, target, depth, temporal_linear, temporal_resolved;
    if (!Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &hdr_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
             &clear, IID_PPV_ARGS(hdr.put())), "CreateHdr") ||
        !Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
             &output_clear, IID_PPV_ARGS(target.put())), "CreateOutput") ||
        !Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
             &depth_clear, IID_PPV_ARGS(depth.put())), "CreateDepth")) return 1;
    if (temporal_output && !Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &hdr_desc,
                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(temporal_linear.put())), "CreateTemporalLinear")) return 1;
    if (history && !Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &hdr_desc,
                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(temporal_resolved.put())), "CreateTemporalResolved")) return 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_srv{}; hdr_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdr_srv.Format = kHdr; hdr_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; hdr_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(hdr.p, &hdr_srv, cpu_srv); cpu_srv.ptr += srv_step;
    const UINT shadow_faces = frame.shadow_size ? static_cast<UINT>(frame.lights.size()*6) : 0;
    ComPtr<ID3D12Resource> shadow_map, shadow_depth;
    if (frame.shadow_size) {
        auto shadow_desc = Image(frame.shadow_size, frame.shadow_size, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        shadow_desc.DepthOrArraySize = static_cast<UINT16>(shadow_faces);
        const auto shadow_depth_desc = Image(frame.shadow_size, frame.shadow_size, 1, DXGI_FORMAT_D32_FLOAT,
                                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE shadow_clear{}; shadow_clear.Format = DXGI_FORMAT_R32_FLOAT; shadow_clear.Color[0] = 1;
        if (!Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &shadow_desc,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, &shadow_clear, IID_PPV_ARGS(shadow_map.put())), "CreateShadowMap") ||
            !Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &shadow_depth_desc,
                  D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(shadow_depth.put())), "CreateShadowDepth")) return 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        srv.TextureCubeArray.MipLevels = 1; srv.TextureCubeArray.NumCubes = static_cast<UINT>(frame.lights.size());
        device->CreateShaderResourceView(shadow_map.p, &srv, cpu_srv); cpu_srv.ptr += srv_step;
    }
    ComPtr<ID3D12Resource> gbuffer[4];
    if (use_gbuffer) {
        for (auto& image : gbuffer) {
            if (!Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &hdr_desc,
                      D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(image.put())), "CreateIndirectBuffer")) return 1;
            device->CreateShaderResourceView(image.p, &hdr_srv, cpu_srv); cpu_srv.ptr += srv_step;
        }
    }
    GpuTexture previous_color, previous_position;
    if (history) {
        if (!UploadHistoryImage(device.p, list.p, frame.width, frame.height, history->color, previous_color) ||
            !UploadHistoryImage(device.p, list.p, frame.width, frame.height, history->position, previous_position)) return 1;
        device->CreateShaderResourceView(previous_color.texture.p, &hdr_srv, cpu_srv); cpu_srv.ptr += srv_step;
        device->CreateShaderResourceView(previous_position.texture.p, &hdr_srv, cpu_srv); cpu_srv.ptr += srv_step;
        device->CreateShaderResourceView(temporal_linear.p, &hdr_srv, cpu_srv); cpu_srv.ptr += srv_step;
    }
    heap_desc = {}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 2+shadow_faces+(use_gbuffer ? 4 : 0)+(temporal_output ? 1 : 0)+(history ? 1 : 0);
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    if (!Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.put())), "CreateRtvHeap")) return 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap_desc.NumDescriptors = frame.shadow_size ? 2 : 1;
    if (!Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(dsv_heap.put())), "CreateDsvHeap")) return 1;
    auto hdr_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto output_rtv = hdr_rtv; output_rtv.ptr += rtv_step;
    auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(hdr.p, nullptr, hdr_rtv); device->CreateRenderTargetView(target.p, nullptr, output_rtv);
    D3D12_CPU_DESCRIPTOR_HANDLE geometry_rtvs[5] = {hdr_rtv};
    if (use_gbuffer) {
        auto handle = hdr_rtv; handle.ptr += UINT64(2+shadow_faces)*rtv_step;
        for (UINT i = 0; i < 4; ++i) {
            geometry_rtvs[i+1] = handle;
            device->CreateRenderTargetView(gbuffer[i].p, nullptr, handle); handle.ptr += rtv_step;
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE temporal_rtv{};
    if (temporal_output) {
        temporal_rtv = hdr_rtv; temporal_rtv.ptr += UINT64(2+shadow_faces+4)*rtv_step;
        device->CreateRenderTargetView(temporal_linear.p, nullptr, temporal_rtv);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE resolved_rtv{};
    if (history) {
        resolved_rtv = temporal_rtv; resolved_rtv.ptr += rtv_step;
        device->CreateRenderTargetView(temporal_resolved.p, nullptr, resolved_rtv);
    }
    device->CreateDepthStencilView(depth.p, nullptr, dsv);
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv{};
    if (frame.shadow_size) {
        shadow_dsv = dsv; shadow_dsv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        device->CreateDepthStencilView(shadow_depth.p, nullptr, shadow_dsv);
        auto face_handle = output_rtv; face_handle.ptr += rtv_step;
        for (UINT i = 0; i < shadow_faces; ++i) {
            D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format = DXGI_FORMAT_R32_FLOAT;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = i; view.Texture2DArray.ArraySize = 1;
            device->CreateRenderTargetView(shadow_map.p, &view, face_handle); face_handle.ptr += rtv_step;
        }
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 row_size = 0, total = 0;
    device->GetCopyableFootprints(&output_desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (rows != frame.height || row_size != UINT64(frame.width)*4 || fp.Footprint.RowPitch < row_size ||
        !total || total > SIZE_MAX || fp.Offset > total || row_size > total - fp.Offset ||
        UINT64(frame.height-1) > (total-fp.Offset-row_size)/fp.Footprint.RowPitch) return 1;
    const auto read_desc = Buffer(total);
    ComPtr<ID3D12Resource> readback;
    if (!Check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &read_desc, D3D12_RESOURCE_STATE_COPY_DEST,
             nullptr, IID_PPV_ARGS(readback.put())), "CreateReadback")) return 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT temporal_fp{}; UINT temporal_rows = 0;
    UINT64 temporal_row_size = 0, temporal_total = 0;
    ComPtr<ID3D12Resource> temporal_color_readback, temporal_position_readback;
    if (temporal_output) {
        device->GetCopyableFootprints(&hdr_desc, 0, 1, 0, &temporal_fp, &temporal_rows, &temporal_row_size, &temporal_total);
        if (temporal_rows != frame.height || temporal_row_size != UINT64(frame.width)*8 ||
            temporal_fp.Footprint.RowPitch < temporal_row_size || !temporal_total || temporal_total > SIZE_MAX ||
            temporal_fp.Offset > temporal_total || temporal_row_size > temporal_total-temporal_fp.Offset ||
            UINT64(frame.height-1) > (temporal_total-temporal_fp.Offset-temporal_row_size)/temporal_fp.Footprint.RowPitch) return 1;
        const auto temporal_read_desc = Buffer(temporal_total);
        if (!Check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &temporal_read_desc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(temporal_color_readback.put())), "CreateTemporalColorReadback") ||
            !Check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &temporal_read_desc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(temporal_position_readback.put())), "CreateTemporalPositionReadback")) return 1;
    }
    const std::size_t vertex_bytes = frame.vertices.size()*sizeof(Vertex);
    const auto vertex_desc = Buffer(vertex_bytes), constant_desc = Buffer((sizeof(Constants)+255)&~std::size_t(255));
    ComPtr<ID3D12Resource> vertices, constants;
    if (!Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
             nullptr, IID_PPV_ARGS(vertices.put())), "CreateVertices") ||
        !Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &constant_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
             nullptr, IID_PPV_ARGS(constants.put())), "CreateConstants")) return 1;
    void* mapped = nullptr; const D3D12_RANGE no_read{0, 0};
    if (!Check(vertices->Map(0, &no_read, &mapped), "MapVertices") || !mapped) return 1;
    std::memcpy(mapped, frame.vertices.data(), vertex_bytes);
    const D3D12_RANGE vertex_written{0, vertex_bytes}; vertices->Unmap(0, &vertex_written);
    Constants cb{};
    std::memcpy(cb.view, frame.view, sizeof(cb.view)); std::memcpy(cb.projection, frame.projection, sizeof(cb.projection));
    cb.size_scale_count[0] = static_cast<float>(frame.width); cb.size_scale_count[1] = static_cast<float>(frame.height);
    cb.size_scale_count[2] = frame.meters_per_unit; cb.size_scale_count[3] = static_cast<float>(frame.lights.size());
    for (unsigned i = 0; i < 3; ++i) { cb.camera_exposure[i] = frame.camera[i]; cb.ambient[i] = frame.ambient[i]; }
    cb.camera_exposure[3] = frame.exposure;
    for (std::size_t i = 0; i < frame.lights.size(); ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            cb.light_position_radius[i][j] = frame.lights[i].position[j];
            cb.light_color_intensity[i][j] = frame.lights[i].color[j];
        }
        cb.light_position_radius[i][3] = frame.lights[i].radius_m;
        cb.light_color_intensity[i][3] = frame.lights[i].intensity;
    }
    if (frame.shadow_size) {
        cb.shadow_settings[0] = frame.shadow_far_m; cb.shadow_settings[1] = frame.shadow_bias_m;
        cb.shadow_settings[2] = frame.shadow_softness; cb.shadow_settings[3] = static_cast<float>(frame.shadow_size);
    }
    cb.jitter[0] = jitter_x; cb.jitter[1] = jitter_y;
    mapped = nullptr;
    if (!Check(constants->Map(0, &no_read, &mapped), "MapConstants") || !mapped) return 1;
    std::memcpy(mapped, &cb, sizeof(cb)); const D3D12_RANGE cb_written{0, sizeof(cb)}; constants->Unmap(0, &cb_written);
    ComPtr<ID3D12Resource> previous_constants;
    if (history) {
        const auto desc = Buffer((sizeof(PreviousConstants)+255)&~std::size_t(255));
        if (!Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(previous_constants.put())), "CreatePreviousConstants")) return 1;
        PreviousConstants previous{};
        std::memcpy(previous.view, history->view, sizeof(previous.view));
        std::memcpy(previous.projection, history->projection, sizeof(previous.projection));
        for (unsigned i = 0; i < 3; ++i) previous.camera_scale[i] = history->camera[i];
        previous.camera_scale[3] = history->scale;
        previous.jitter_weight[0] = history->jitter[0]; previous.jitter_weight[1] = history->jitter[1];
        previous.jitter_weight[2] = history_weight;
        mapped = nullptr;
        if (!Check(previous_constants->Map(0, &no_read, &mapped), "MapPreviousConstants") || !mapped) return 1;
        std::memcpy(mapped, &previous, sizeof(previous));
        const D3D12_RANGE written{0, sizeof(previous)}; previous_constants->Unmap(0, &written);
    }
    std::wstring current_output(output);
    std::uint64_t render_index = 0;
    if (variant) std::printf("variant_gpu_resources=shared textures=%zu\n", frame.textures.size());
    for (;;) {
        if (render_index) {
            if (!Check(allocator->Reset(), "ResetAllocator") || !Check(list->Reset(allocator.p, pipeline.p), "ResetList")) return 1;
            Transition(list.p, target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Transition(list.p, hdr.p, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (frame.shadow_size)
                Transition(list.p, shadow_map.p, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (use_gbuffer) for (auto& image : gbuffer)
                Transition(list.p, image.p, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        const std::uint8_t selected = variant && variant->variants->ReplacementSelected() ? 2 : 1;
        list->SetGraphicsRootSignature(root.p);
        ID3D12DescriptorHeap* heaps[] = {srv_heap.p}; list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
        const D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(), static_cast<UINT>(vertex_bytes), sizeof(Vertex)};
        list->IASetVertexBuffers(0, 1, &vb); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto gpu_base = srv_heap->GetGPUDescriptorHandleForHeapStart();
        if (frame.shadow_size) {
            // Direct3D cubemap order and per-face right/up axes, in world coordinates.
            static const float axes[6][3][3] = {
                {{0,0,-1}, {0,1,0}, {1,0,0}}, {{0,0,1}, {0,1,0}, {-1,0,0}},
                {{1,0,0}, {0,0,-1}, {0,1,0}}, {{1,0,0}, {0,0,1}, {0,-1,0}},
                {{1,0,0}, {0,1,0}, {0,0,1}}, {{-1,0,0}, {0,1,0}, {0,0,-1}}};
            const D3D12_VIEWPORT shadow_viewport{0, 0, static_cast<float>(frame.shadow_size),
                                                  static_cast<float>(frame.shadow_size), 0, 1};
            const D3D12_RECT shadow_scissor{0, 0, static_cast<LONG>(frame.shadow_size), static_cast<LONG>(frame.shadow_size)};
            const float shadow_clear[4] = {1, 0, 0, 0};
            list->SetPipelineState(shadow_pipeline.p);
            list->RSSetViewports(1, &shadow_viewport); list->RSSetScissorRects(1, &shadow_scissor);
            for (std::size_t light = 0; light < frame.lights.size(); ++light) for (UINT face = 0; face < 6; ++face) {
                auto rtv = output_rtv; rtv.ptr += UINT64(1+light*6+face)*rtv_step;
                list->OMSetRenderTargets(1, &rtv, FALSE, &shadow_dsv);
                list->ClearRenderTargetView(rtv, shadow_clear, 0, nullptr);
                list->ClearDepthStencilView(shadow_dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
                float shadow_constants[20]{};
                for (UINT k = 0; k < 3; ++k) {
                    shadow_constants[k] = frame.lights[light].position[k];
                    for (UINT axis = 0; axis < 3; ++axis) shadow_constants[4+axis*4+k] = axes[face][axis][k];
                }
                shadow_constants[3] = frame.meters_per_unit;
                shadow_constants[16] = frame.shadow_near_m; shadow_constants[17] = frame.shadow_far_m;
                list->SetGraphicsRoot32BitConstants(7, 20, shadow_constants, 0);
                for (std::size_t i = 0; i < frame.draws.size(); ++i) {
                    const auto& draw = frame.draws[i];
                    if ((!variant || variant->draws[i] == selected) && (draw.shadow_mask & (1u << light)))
                        list->DrawInstanced(draw.vertex_count, 1, draw.first_vertex, 0);
                }
                for (std::size_t i = 0; i < frame.offline_casters.size(); ++i) {
                    const auto& caster = frame.offline_casters[i];
                    if ((!variant || variant->casters[i] == selected) && (caster.shadow_mask & (1u << light)))
                        list->DrawInstanced(caster.vertex_count, 1, caster.first_vertex, 0);
                }
            }
            Transition(list.p, shadow_map.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto shadow_handle = gpu_base; shadow_handle.ptr += UINT64(frame.textures.size()*2+1)*srv_step;
            list->SetGraphicsRootDescriptorTable(6, shadow_handle);
            list->SetPipelineState(pipeline.p);
        }
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height), frame.min_depth, frame.max_depth};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(frame.width), static_cast<LONG>(frame.height)};
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(use_gbuffer ? 5 : 1, geometry_rtvs, FALSE, &dsv);
        for (UINT i = 0; i < (use_gbuffer ? 5u : 1u); ++i) list->ClearRenderTargetView(geometry_rtvs[i], clear.Color, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        for (std::size_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
            if (variant && variant->draws[draw_index] != selected) continue;
            const auto& draw = frame.draws[draw_index];
            if (frame.has_metallic) {
                const float material[4] = {draw.emission[0], draw.emission[1], draw.emission[2], draw.metallic};
                list->SetGraphicsRoot32BitConstants(12, 4, material, 0);
            } else if (frame.has_emission) list->SetGraphicsRoot32BitConstants(12, 3, draw.emission, 0);
            for (UINT i = 0; i < 4; ++i) {
                auto handle = gpu_base; handle.ptr += (UINT64(draw.textures[i])*2+(i == 0 ? 1 : 0))*srv_step;
                list->SetGraphicsRootDescriptorTable(i+1, handle);
            }
            list->DrawInstanced(draw.vertex_count, 1, draw.first_vertex, 0);
        }
        Transition(list.p, hdr.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (use_gbuffer) {
            auto handle = gpu_base; handle.ptr += UINT64(frame.textures.size()*2+1+(frame.shadow_size ? 1 : 0))*srv_step;
            for (UINT i = 0; i < 4; ++i) {
                Transition(list.p, gbuffer[i].p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->SetGraphicsRootDescriptorTable(i+8, handle); handle.ptr += srv_step;
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE tone_rtvs[] = {output_rtv, temporal_rtv};
        list->OMSetRenderTargets(temporal_output ? 2 : 1, tone_rtvs, FALSE, nullptr);
        list->SetPipelineState(tone_pipeline.p);
        auto hdr_handle = gpu_base; hdr_handle.ptr += UINT64(frame.textures.size()*2)*srv_step;
        list->SetGraphicsRootDescriptorTable(5, hdr_handle);
        list->DrawInstanced(3, 1, 0, 0);
        if (history) {
            Transition(list.p, temporal_linear.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            D3D12_CPU_DESCRIPTOR_HANDLE resolved_rtvs[] = {output_rtv, resolved_rtv};
            list->OMSetRenderTargets(2, resolved_rtvs, FALSE, nullptr);
            list->SetPipelineState(temporal_pipeline.p);
            list->SetGraphicsRootConstantBufferView(13, previous_constants->GetGPUVirtualAddress());
            auto handle = gpu_base;
            handle.ptr += UINT64(frame.textures.size()*2+1+(frame.shadow_size ? 1 : 0)+4)*srv_step;
            for (UINT i = 0; i < 3; ++i) {
                list->SetGraphicsRootDescriptorTable(i+14, handle); handle.ptr += srv_step;
            }
            list->DrawInstanced(3, 1, 0, 0);
        }
        if (temporal_output) {
            if (history) Transition(list.p, temporal_resolved.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            else Transition(list.p, temporal_linear.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(list.p, gbuffer[0].p, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = temporal_fp;
            D3D12_TEXTURE_COPY_LOCATION source{}; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = temporal_color_readback.p; source.pResource = history ? temporal_resolved.p : temporal_linear.p;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            destination.pResource = temporal_position_readback.p; source.pResource = gbuffer[0].p;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        Transition(list.p, target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.p; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = target.p; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (render_index == UINT64_MAX || !Check(list->Close(), "CloseList")) return 1;
        ID3D12CommandList* lists[] = {list.p}; queue->ExecuteCommandLists(1, lists);
        const UINT64 fence_target = render_index + 1;
        if (FAILED(queue->Signal(fence.p, fence_target))) AbortInFlight("Queue signal failed");
        UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) AbortInFlight("Device removed");
        if (completed < fence_target && (FAILED(fence->SetEventOnCompletion(fence_target, event.p)) || WaitForSingleObject(event.p, 10000) != WAIT_OBJECT_0))
            AbortInFlight("Fence wait failed");
        completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < fence_target) AbortInFlight("Fence did not complete");
        if (!Check(device->GetDeviceRemovedReason(), "Device status")) return 1;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(total)}; mapped = nullptr;
        if (!Check(readback->Map(0, &read, &mapped), "MapReadback") || !mapped) return 1;
        const auto* pixels = static_cast<const std::uint8_t*>(mapped);
        std::size_t changed = 0;
        for (UINT y = 0; y < frame.height; ++y) for (UINT x = 0; x < frame.width; ++x) {
            const auto* pixel = pixels+fp.Offset+y*fp.Footprint.RowPitch+x*4;
            if (pixel[0] || pixel[1] || pixel[2]) ++changed;
        }
        const bool ok = changed && WriteImage(current_output.c_str(), pixels, fp, frame.width, frame.height);
        const D3D12_RANGE no_write{0, 0}; readback->Unmap(0, &no_write);
        if (!changed) { std::fprintf(stderr, "PBR preview produced no visible pixels\n"); return 1; }
        if (!ok) { std::fprintf(stderr, "image write failed; partial file may remain\n"); return 1; }
        if (temporal_output) {
            void* color = nullptr; void* position = nullptr;
            const D3D12_RANGE temporal_read{0, static_cast<SIZE_T>(temporal_total)};
            if (!Check(temporal_color_readback->Map(0, &temporal_read, &color), "MapTemporalColor") || !color) return 1;
            if (!Check(temporal_position_readback->Map(0, &temporal_read, &position), "MapTemporalPosition") || !position) {
                temporal_color_readback->Unmap(0, &no_write); return 1;
            }
            const std::uint32_t flags = (indirect ? 1u : 0u) | (gtao ? 2u : 0u) |
                                        (volumetric ? 4u : 0u) | (reflections ? 8u : 0u) | (filmic ? 16u : 0u);
            const bool saved = WriteTemporal(temporal_output, frame, digest, jitter_x, jitter_y, flags,
                                             static_cast<const std::uint8_t*>(color), static_cast<const std::uint8_t*>(position), temporal_fp);
            temporal_position_readback->Unmap(0, &no_write); temporal_color_readback->Unmap(0, &no_write);
            if (!saved) { std::fprintf(stderr, "temporal buffer write failed; partial file may remain\n"); return 1; }
        }
        std::size_t visible_vertices = 0, visible_draws = 0, caster_vertices = 0;
        for (std::size_t i = 0; i < frame.draws.size(); ++i) if (!variant || variant->draws[i] == selected) {
            visible_vertices += frame.draws[i].vertex_count; ++visible_draws;
        }
        for (std::size_t i = 0; i < frame.offline_casters.size(); ++i)
            if (!variant || variant->casters[i] == selected) caster_vertices += frame.offline_casters[i].vertex_count;
        std::printf("sceneir_pbr_dx12=ok textures=%zu draws=%zu triangles=%zu offline_caster_triangles=%zu lights=%zu changed_pixels=%zu\n",
                    frame.textures.size(), visible_draws, visible_vertices/3, caster_vertices/3, frame.lights.size(), changed);
        if (!variant) return 0;
        if (variant->stream) {
            if (!render_index) std::printf("variant_stream=ready temporal_history=reset\n");
            else std::printf("variant_selected=%s temporal_history=reset\n", selected == 2 ? "replacement" : "original");
            std::fflush(stdout);
            std::wstring line;
            if (!std::getline(std::wcin, line)) return std::wcin.eof() ? 0 : 2;
            if (line.size() > 32767) return 2;
            if (line == L"quit") return 0;
            const auto split = line.find(L' ');
            if (split == std::wstring::npos || split + 1 == line.size()) return 2;
            const auto command = line.substr(0, split);
            if (command == L"frame") {
                const auto tab = line.find(L'\t', split+1);
                if (tab == std::wstring::npos || tab == split+1 || tab+1 == line.size() ||
                    line.find(L'\t', tab+1) != std::wstring::npos) return 2;
                const auto path = line.substr(split+1, tab-split-1);
                if (!ReadCameraUpdate(path.c_str(), *variant->variants)) return 2;
                const auto& camera_frame = variant->variants->Current();
                std::memcpy(cb.view, camera_frame.view, sizeof(cb.view));
                std::memcpy(cb.projection, camera_frame.projection, sizeof(cb.projection));
                for (unsigned i = 0; i < 3; ++i) cb.camera_exposure[i] = camera_frame.camera[i];
                mapped = nullptr;
                if (!Check(constants->Map(0, &no_read, &mapped), "MapUpdatedCamera") || !mapped) return 1;
                std::memcpy(mapped, &cb, sizeof(cb));
                const D3D12_RANGE written{0, sizeof(cb)}; constants->Unmap(0, &written);
                current_output = line.substr(tab+1);
                std::printf("variant_camera=updated temporal_history=reset\n");
                std::fflush(stdout);
            } else {
                if ((command != L"original" && command != L"replacement") ||
                    !variant->variants->Select(variant->key, command == L"replacement")) return 2;
                current_output = line.substr(split+1);
            }
        } else if (render_index == 0) {
            if (!variant->variants->Select(variant->key, true)) return 2;
            current_output = variant->replacement_output;
        } else if (render_index == 1) {
            if (!variant->variants->Select(variant->key, false)) return 2;
            current_output = variant->restored_output;
        } else {
            std::printf("variant_cycle=original,replacement,original temporal_history=reset\n");
            return 0;
        }
        ++render_index;
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto usage = []() {
        std::fwprintf(stderr, L"usage: t3render_scene_pbr_d3d12 <pbr.bin> --output <new-image.ppm> [effects] [--temporal-output <new.bin> [--jitter <x> <y>]] [--temporal-input <history.bin> --previous-packet <pbr.bin> [--history-weight <0..0.95>]] [(--variant-cycle <replacement.ppm> <restored.ppm> | --variant-stream) --resource-key <key>]\n");
        return 2;
    };
    if (argc < 4 || argc > 20 || std::wcscmp(argv[2], L"--output") || !argv[3][0]) return usage();
    bool indirect = false, gtao = false, volumetric = false, reflections = false, filmic = false;
    bool jitter_set = false, weight_set = false; float jitter_x = 0, jitter_y = 0, history_weight = .8f;
    const wchar_t* temporal_output = nullptr;
    const wchar_t* temporal_input = nullptr; const wchar_t* previous_packet = nullptr;
    const wchar_t* variant_output = nullptr; const wchar_t* restored_output = nullptr; const wchar_t* variant_key = nullptr;
    bool variant_stream = false;
    for (int i = 4; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--indirect") && !indirect) indirect = true;
        else if (!std::wcscmp(argv[i], L"--gtao") && !gtao) gtao = true;
        else if (!std::wcscmp(argv[i], L"--volumetric") && !volumetric) volumetric = true;
        else if (!std::wcscmp(argv[i], L"--reflections") && !reflections) reflections = true;
        else if (!std::wcscmp(argv[i], L"--filmic") && !filmic) filmic = true;
        else if (!std::wcscmp(argv[i], L"--temporal-output") && !temporal_output && i+1 < argc && argv[i+1][0])
            temporal_output = argv[++i];
        else if (!std::wcscmp(argv[i], L"--temporal-input") && !temporal_input && i+1 < argc && argv[i+1][0])
            temporal_input = argv[++i];
        else if (!std::wcscmp(argv[i], L"--previous-packet") && !previous_packet && i+1 < argc && argv[i+1][0])
            previous_packet = argv[++i];
        else if (!std::wcscmp(argv[i], L"--variant-cycle") && !variant_output && i+2 < argc && argv[i+1][0] && argv[i+2][0]) {
            variant_output = argv[++i]; restored_output = argv[++i];
        }
        else if (!std::wcscmp(argv[i], L"--variant-stream") && !variant_stream) variant_stream = true;
        else if (!std::wcscmp(argv[i], L"--resource-key") && !variant_key && i+1 < argc && argv[i+1][0])
            variant_key = argv[++i];
        else if (!std::wcscmp(argv[i], L"--history-weight") && !weight_set && i+1 < argc) {
            wchar_t* end = nullptr; const wchar_t* value = argv[++i]; history_weight = std::wcstof(value, &end);
            if (!end || end == value || *end || !std::isfinite(history_weight) || history_weight < 0 || history_weight > .95f) return usage();
            weight_set = true;
        }
        else if (!std::wcscmp(argv[i], L"--jitter") && !jitter_set && i+2 < argc) {
            wchar_t* end_x = nullptr; wchar_t* end_y = nullptr;
            const wchar_t* x = argv[++i]; const wchar_t* y = argv[++i];
            jitter_x = std::wcstof(x, &end_x); jitter_y = std::wcstof(y, &end_y);
            if (!end_x || !end_y || end_x == x || end_y == y || *end_x || *end_y || !std::isfinite(jitter_x) || !std::isfinite(jitter_y) ||
                jitter_x < -.5f || jitter_x > .5f || jitter_y < -.5f || jitter_y > .5f) return usage();
            jitter_set = true;
        }
        else return usage();
    }
    if ((jitter_set && !temporal_output) || (temporal_output && !std::wcscmp(temporal_output, argv[3])) ||
        ((temporal_input || previous_packet || weight_set) && !(temporal_input && previous_packet && temporal_output)) ||
        (temporal_input && temporal_output && !std::wcscmp(temporal_input, temporal_output)) ||
        ((variant_output != nullptr || variant_stream) != (variant_key != nullptr)) || (variant_output && variant_stream) ||
        ((variant_output || variant_stream) && (temporal_output || temporal_input || jitter_set || weight_set || previous_packet)) ||
        (variant_output && (!std::wcscmp(argv[3], variant_output) || !std::wcscmp(argv[3], restored_output) ||
                            !std::wcscmp(variant_output, restored_output)))) return usage();
    if (variant_output || variant_stream) {
        std::string key;
        for (const wchar_t* p = variant_key; *p; ++p) {
            if (*p < 33 || *p > 126 || key.size() >= kMaxVariantKeyBytes) return usage();
            key.push_back(static_cast<char>(*p));
        }
        Variants variants;
        if (!ReadVariants(argv[1], variants) || !variants.Select(key, false)) {
            std::fprintf(stderr, "variant resource key does not match\n"); return 2;
        }
        Frame combined;
        VariantRun run;
        run.variants = &variants; run.key = key; run.stream = variant_stream;
        run.replacement_output = variant_output; run.restored_output = restored_output;
        if (!variants.BuildCombined(combined, run.draws, run.casters)) {
            std::fprintf(stderr, "cannot build bounded combined variants\n"); return 2;
        }
        return Render(combined, argv[3], indirect, gtao, volumetric, reflections, filmic,
                      nullptr, 0, 0, nullptr, nullptr, 0, &run);
    }
    Frame frame;
    std::uint8_t digest[32]{};
    if (!ReadPacket(argv[1], frame, temporal_output ? digest : nullptr)) return 2;
    t3render::temporal::History history;
    if (temporal_input) {
        Frame previous; std::uint8_t previous_digest[32]{};
        if (!ReadPacket(previous_packet, previous, previous_digest)) return 2;
        FILE* file = _wfopen(temporal_input, L"rb");
        if (!file) { std::fprintf(stderr, "cannot open temporal history\n"); return 2; }
        constexpr std::size_t max_history = 208 + std::size_t(2048)*2048*16;
        if (_fseeki64(file, 0, SEEK_END)) { std::fclose(file); return 2; }
        const auto length = _ftelli64(file);
        if (length < 208 || static_cast<std::uint64_t>(length) > max_history || _fseeki64(file, 0, SEEK_SET)) {
            std::fclose(file); std::fprintf(stderr, "invalid temporal history size\n"); return 2;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        const std::size_t size = std::fread(bytes.data(), 1, bytes.size(), file);
        const bool error = size != bytes.size() || std::fgetc(file) != EOF || std::ferror(file) != 0;
        const bool close_error = std::fclose(file) != 0;
        const std::uint32_t flags = (indirect ? 1u : 0u) | (gtao ? 2u : 0u) |
                                    (volumetric ? 4u : 0u) | (reflections ? 8u : 0u) | (filmic ? 16u : 0u);
        if (error || close_error || !t3render::temporal::ParseHistory(bytes.data(), size, previous, frame, previous_digest, flags, history)) {
            std::fprintf(stderr, "invalid or incompatible temporal history\n"); return 2;
        }
        std::printf("temporal_frame_order=unverified; validate SceneIR provenance separately\n");
    }
    return Render(frame, argv[3], indirect, gtao, volumetric, reflections, filmic, temporal_output, jitter_x, jitter_y, digest,
                  temporal_input ? &history : nullptr, history_weight);
}
