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
TextureCubeArray shadow_maps:register(t5);SamplerState shadow_sampler:register(s1);
cbuffer Material:register(b1){float4 color;}
cbuffer Lighting:register(b3){float4 camera_scale;float4 options;
    float4 position_range[16];float4 color_intensity[16];float4 direction_outer[16];float4 light_params[16];};
float visibility(float3 worldpos,float3 n,float3 l,uint i){
    if(i>=(uint)options.z)return 1;
    float3 ray=worldpos-position_range[i].xyz;float ray_length=length(ray);
    float distance=ray_length*camera_scale.w, far_m=position_range[i].w;
    if(distance>=far_m||ray_length<0.000001)return 1;
    float3 direction=ray/ray_length;
    float3 axis=abs(direction.z)<0.95?float3(0,0,1):float3(0,1,0);
    float3 t=normalize(cross(axis,direction)),b=cross(direction,t);
    float radius=min(0.15,light_params[i].x/max(distance,0.01));
    radius=max(radius,1.0/256.0);
    float slope=1+2*(1-saturate(dot(n,l)));
    float pn=dot(n,direction),pc=abs(pn);
    float texel_slope=min(4,sqrt(saturate(1-pc*pc))/max(pc,0.1));
    float bias=max(0.001,far_m/256.0*0.35)*slope+distance*(2.0/256.0)*texel_slope;
    // A bounded blocker search estimates penumbra size from light/occluder/
    // receiver separation. It remains an approximation on a partial scene.
    float search=min(0.15,max(2*radius,2.0/256.0));
    float blocker_sum=0,blocker_count=0;
    [unroll] for(int sy=-1;sy<=1;++sy)[unroll] for(int sx=-1;sx<=1;++sx){
        float3 sample_dir=normalize(direction+(t*sx+b*sy)*search);
        float pd=dot(n,sample_dir);
        float plane=(abs(pd)>0.05&&pn*pd>0)?clamp(pn/pd,0.5,2):1;
        float blocker=shadow_maps.SampleLevel(shadow_sampler,float4(sample_dir,(float)i),0).r*far_m;
        if(blocker<distance*plane-bias){blocker_sum+=blocker/plane;blocker_count+=1;}
    }
    if(blocker_count==0)return 1;
    float blocker=max(blocker_sum/blocker_count,0.001);
    radius=min(0.15,light_params[i].x*max(distance-blocker,0)/(blocker*max(distance,0.01)));
    radius=max(radius,1.0/256.0);
    float visible=0;
    [unroll] for(int y=-2;y<=2;++y)[unroll] for(int x=-2;x<=2;++x){
        float3 sample_dir=normalize(direction+(t*x+b*y)*(radius*0.5));
        float pd=dot(n,sample_dir);
        float plane=(abs(pd)>0.05&&pn*pd>0)?clamp(pn/pd,0.5,2):1;
        float receiver=distance*plane-bias;
        float blocker=shadow_maps.SampleLevel(shadow_sampler,float4(sample_dir,(float)i),0).r*far_m;
        visible+=receiver<=blocker?1:0;
    }
    return visible/25;
}
float4 main(float4 uv:TEXCOORD0,float3 worldpos:TEXCOORD1,float3 normal:TEXCOORD2):SV_Target{
    const float pi=3.14159265359;
    float3 albedo=color.rgb;float roughness=0.7;
#if TEXTURED
    albedo=albedo_tex.Sample(samp,uv.xy).rgb; // sRGB SRV decodes before filtering
    roughness=clamp(roughness_tex.Sample(samp,uv.xy).r,0.04,1);
#endif
    float3 n=normalize(normal);
    float3 geometric=cross(ddx(worldpos),ddy(worldpos));
    float g2=dot(geometric,geometric);
    geometric=g2>0.0000000001?geometric*rsqrt(g2):n;
    if(dot(geometric,n)<0)geometric=-geometric;
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
            result+=(diffuse+spec)*radiance*nl*cutoff*cone*visibility(worldpos,geometric,l,i);
        }
    }
    // AO belongs to indirect light. There is no verified indirect term yet.
    return float4(min(result,65504),1);
})";
inline constexpr char shadow_vertex[] = R"(
cbuffer Instance:register(b2){row_major float4x4 world;}
cbuffer Shadow:register(b4){float4 light_scale;float4 face_right;float4 face_up;float4 face_forward;float4 clip_range;}
struct Output{float3 worldpos:TEXCOORD0;float4 position:SV_Position;};
Output main(float3 position:POSITION){Output o;float3 wp=mul(float4(position,1),world).xyz;
    float3 p=(wp-light_scale.xyz)*light_scale.w;float z=dot(p,face_forward.xyz);
    o.position=float4(dot(p,face_right.xyz),dot(p,face_up.xyz),
                      (z*clip_range.y-clip_range.x*clip_range.y)/(clip_range.y-clip_range.x),z);
    o.worldpos=wp;return o;}
)";
inline constexpr char shadow_pixel[] = R"(
cbuffer Shadow:register(b4){float4 light_scale;float4 face_right;float4 face_up;float4 face_forward;float4 clip_range;}
float main(float3 worldpos:TEXCOORD0):SV_Target{
    return saturate(length((worldpos-light_scale.xyz)*light_scale.w)/clip_range.y);
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
