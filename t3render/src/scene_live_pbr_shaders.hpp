#pragma once

// The live consumer's experimental direct-light pass. SceneIR uses row
// vectors; no tangent-space convention has yet been established for TDS.
namespace t3render::live::shaders {
inline constexpr char vertex[] = R"(
cbuffer Camera:register(b0){row_major float4x4 view;row_major float4x4 proj;float2 size;}
cbuffer Instance:register(b2){row_major float4x4 world;}
struct Output{float4 uv:TEXCOORD0;float3 worldpos:TEXCOORD1;float3 normal:TEXCOORD2;float4 position:SV_Position;};
Output main(float3 p:POSITION,float2 uv:TEXCOORD0,float3 n:NORMAL){
    Output o;float4 wp=mul(float4(p,1),world);
    float3 a=world[0].xyz,b=world[1].xyz,c=world[2].xyz;
    float det=dot(a,cross(b,c));
    o.normal=(n.x*cross(b,c)+n.y*cross(c,a)+n.z*cross(a,b))/det;
    o.worldpos=wp.xyz;o.uv=float4(uv,0,0);
    o.position=mul(mul(wp,view),proj);
    o.position.x+=o.position.w/size.x;o.position.y-=o.position.w/size.y;return o;
})";
// TEXTURED and TEST_NORMAL_MAP are injected by the caller. The latter is
// confined to synthetic fixtures until the real normal-map convention is known.
inline constexpr char pixel[] = R"(
Texture2D albedo_tex:register(t0);Texture2D normal_tex:register(t1);
Texture2D roughness_tex:register(t2);Texture2D ao_tex:register(t3);SamplerState samp:register(s0);
cbuffer Material:register(b1){float4 color;}
cbuffer Lighting:register(b3){float4 camera_scale;float4 options;
    float4 position_range[16];float4 color_intensity[16];float4 direction_outer[16];float4 light_params[16];};
float4 main(float4 uv:TEXCOORD0,float3 worldpos:TEXCOORD1,float3 normal:TEXCOORD2):SV_Target{
    const float pi=3.14159265359;
    float3 albedo=color.rgb;float roughness=0.7;
#if TEXTURED
    albedo=albedo_tex.Sample(samp,uv.xy).rgb; // sRGB SRV decodes before filtering
    roughness=clamp(roughness_tex.Sample(samp,uv.xy).r,0.04,1);
#endif
    float3 n=normalize(normal);
#if TEXTURED && TEST_NORMAL_MAP
    // Fixture-only convention: tangent X follows U, normal-map Y follows V.
    if(options.y>0.5){
        float3 px=ddx(worldpos),py=ddy(worldpos);float2 ux=ddx(uv.xy),uy=ddy(uv.xy);
        float3 t=px*uy.y-py*ux.y,b=py*ux.x-px*uy.x;
        float det=ux.x*uy.y-ux.y*uy.x;
        if(abs(det)>0.0000001){
            t=t/det;t-=n*dot(n,t);b=b/det;b-=n*dot(n,b);
            if(dot(t,t)>0.0000001&&dot(b,b)>0.0000001){
                t=normalize(t);b=normalize(b-t*dot(t,b));
                float3 mapped=normal_tex.Sample(samp,uv.xy).xyz*2-1;
                n=normalize(t*mapped.x+b*mapped.y+n*mapped.z);
            }
        }
    }
#endif
    float3 v=normalize(camera_scale.xyz-worldpos);float nv=saturate(dot(n,v));
    float alpha=roughness*roughness,a2=alpha*alpha,k=(roughness+1)*(roughness+1)/8;
    float3 result=0;
    [loop] for(uint i=0;i<(uint)options.x;i++){
        float3 delta=(position_range[i].xyz-worldpos)*camera_scale.w;
        float d2=dot(delta,delta);float3 l=delta*rsqrt(max(d2,0.00000001));
        float nl=saturate(dot(n,l));
        if(nl>0&&nv>0){
            float3 h=normalize(v+l);float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
            float denom=nh*nh*(a2-1)+1;
            float d=a2/max(pi*denom*denom,0.00000001);
            float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));
            float3 fresnel=0.04+0.96*pow(1-vh,5);
            float3 spec=d*g*fresnel/max(4*nv*nl,0.00000001);
            float3 diffuse=(1-fresnel)*albedo/pi;
            float dist=sqrt(d2);float range=position_range[i].w;
            float cutoff=saturate(1-pow(dist/range,4));cutoff*=cutoff;
            float cone=1;
            if(light_params[i].z>0.5){
                float q=saturate((dot(direction_outer[i].xyz,-l)-direction_outer[i].w)/
                                 max(light_params[i].y-direction_outer[i].w,0.000001));
                cone=q*q*(3-2*q);
            }
            float source=light_params[i].x;
            float3 radiance=color_intensity[i].rgb*color_intensity[i].w/max(d2,max(source*source,0.000001));
            result+=(diffuse+spec)*radiance*nl*cutoff*cone;
        }
    }
    // AO belongs to indirect light. There is no verified indirect term yet.
    return float4(min(result,65504),1);
})";
inline constexpr char tone_vertex[] = R"(
struct Output{float2 uv:TEXCOORD0;float4 position:SV_Position;};
Output main(uint id:SV_VertexID){Output o;float2 p=float2((id<<1)&2,id&2);
    o.uv=p;o.position=float4(p.x*2-1,1-p.y*2,0,1);return o;}
)";
inline constexpr char tone_pixel[] = R"(
Texture2D hdr:register(t4);SamplerState samp:register(s0);
float4 main(float2 uv:TEXCOORD0):SV_Target{
    float3 x=max(hdr.Sample(samp,uv).rgb,0);
    x=saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14));
    x=lerp(12.92*x,1.055*pow(x,1/2.4)-0.055,step(0.0031308,x));
    return float4(x,1);
})";
} // namespace t3render::live::shaders
