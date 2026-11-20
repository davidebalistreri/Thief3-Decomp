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
    float4 position_range[16];float4 color_intensity[16];float4 direction_outer[16];float4 light_params[16];
    row_major float4x4 gi_view;row_major float4x4 gi_projection;float4 post_process;};
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
#if LIVE_GBUFFER
struct Output{float4 color:SV_Target0;float4 position:SV_Target1;float4 normal:SV_Target2;
              float4 diffuse:SV_Target3;float4 albedo:SV_Target4;};
Output main(float4 uv:TEXCOORD0,float3 worldpos:TEXCOORD1,float3 normal:TEXCOORD2){
#else
float4 main(float4 uv:TEXCOORD0,float3 worldpos:TEXCOORD1,float3 normal:TEXCOORD2):SV_Target{
#endif
    // HDR alpha marks visible emissive pixels for the local flame glow.
    if(color.a>1){
#if LIVE_GBUFFER
        Output emitted;emitted.color=float4(min(color.rgb*(color.a-1),65504),2);
        emitted.position=0;emitted.normal=0;emitted.diffuse=0;emitted.albedo=0;return emitted;
#else
        return float4(min(color.rgb*(color.a-1),65504),2);
#endif
    }
    const float pi=3.14159265359;
    float3 albedo=color.rgb;float roughness=0.7;float ao=1;float metallic=0;
#if TEXTURED
    float2 material_uv=uv.xy,uv_dx=ddx(uv.xy),uv_dy=ddy(uv.xy);
    // Negative alpha is an internal draw marker for the opt-in head wall.
    // The height map is packed into AO alpha on upload; its RGB is unchanged.
    if(color.a < -1.5 && color.a > -2.5){
        float3 px=ddx(worldpos),py=ddy(worldpos);
        float det=uv_dx.x*uv_dy.y-uv_dx.y*uv_dy.x;
        if(abs(det)>1e-10){
            float3 tu=(px*uv_dy.y-py*uv_dx.y)/det,tv=(py*uv_dx.x-px*uv_dy.x)/det;
            float a=dot(tu,tu),b=dot(tu,tv),c=dot(tv,tv),gram=a*c-b*b;
            float3 face=cross(tu,tv),view=normalize(camera_scale.xyz-worldpos);
            float face2=dot(face,face);
            if(gram>1e-10 && face2>1e-10){
                face*=rsqrt(face2);float nv=abs(dot(face,view));
                if(nv>0.08){
                    float2 slope=float2(dot(view,tu)*c-dot(view,tv)*b,
                                        dot(view,tv)*a-dot(view,tu)*b)/gram;
                    float2 delta=slope*(0.05/camera_scale.w)/max(nv,0.2);
                    delta*=min(1,0.20/max(length(delta),1e-6));
                    const int layers=24;float layer=0,previous_depth=0;
                    float2 step_uv=delta/layers,current=material_uv,previous=current;
                    float depth=1-ao_tex.SampleGrad(samp,current,uv_dx,uv_dy).a;
                    [loop] for(int i=0;i<layers && layer<depth;i++){
                        previous=current;previous_depth=depth;
                        current-=step_uv;layer+=1.0/layers;
                        depth=1-ao_tex.SampleGrad(samp,current,uv_dx,uv_dy).a;
                    }
                    float after=depth-layer,before=previous_depth-(layer-1.0/layers);
                    float weight=abs(before-after)>1e-6?saturate(after/(after-before)):0;
                    material_uv=lerp(current,previous,weight);
                }
            }
        }
    }
    albedo=albedo_tex.SampleGrad(samp,material_uv,uv_dx,uv_dy).rgb; // sRGB SRV
    // The wool source is unusually dark even after the v0.1 catalog lift.
    // Adjust its diffuse reflectance in linear space, not the light or gamma.
    if(color.a==-8 || color.a==-9)albedo=saturate(albedo*2.0);
    roughness=clamp(roughness_tex.SampleGrad(samp,material_uv,uv_dx,uv_dy).r,0.04,1);
    // The generated candlestick has metal albedo but no dedicated auxiliary
    // maps; do not interpret the borrowed wood roughness as metal.
    if(color.a==-6){roughness=0.35;metallic=1;}
#if LIVE_GBUFFER
    ao=saturate(ao_tex.SampleGrad(samp,material_uv,uv_dx,uv_dy).r);
#endif
#endif
    float3 n=normalize(normal);
    float3 geometric=cross(ddx(worldpos),ddy(worldpos));
    float g2=dot(geometric,geometric);
    geometric=g2>0.0000000001?geometric*rsqrt(g2):n;
    if(dot(geometric,n)<0)geometric=-geometric;
#if TEXTURED
    // The wall, floor and bed textiles have generated OpenGL normal maps.
    // Derivatives also preserve the handedness of mirrored UV islands.
    if(options.y>0.5 || (color.a<0 && color.a>-2.5) || color.a==-4 || color.a==-5 ||
       color.a==-8 || color.a==-9 || color.a==-10 || color.a==-11){
        float3 px=ddx(worldpos),py=ddy(worldpos);float2 ux=ddx(uv.xy),uy=ddy(uv.xy);
        float3 t=px*uy.y-py*ux.y,b=py*ux.x-px*uy.x;
        float det=ux.x*uy.y-ux.y*uy.x;
        if(abs(det)>0.0000001){
            t=t/det;t-=n*dot(n,t);b=b/det;b-=n*dot(n,b);
            if(dot(t,t)>0.0000001&&dot(b,b)>0.0000001){
                t=normalize(t);b=normalize(b-t*dot(t,b));
                float3 mapped=normal_tex.SampleGrad(samp,material_uv,uv_dx,uv_dy).xyz*2-1;
                n=normalize(t*mapped.x+b*mapped.y+n*mapped.z);
            }
        }
    }
#endif
    float3 v=normalize(camera_scale.xyz-worldpos);float nv=saturate(dot(n,v));
    float alpha=roughness*roughness,a2=alpha*alpha,k=(roughness+1)*(roughness+1)/8;
    // A nonzero value is used only by the explicitly artistic candle preview.
    float3 result=albedo*options.w;
#if LIVE_GBUFFER
    float3 direct_diffuse=0;
#endif
    [loop] for(uint i=0;i<(uint)options.x;i++){
        float3 delta=(position_range[i].xyz-worldpos)*camera_scale.w;
        float d2=dot(delta,delta);float3 l=delta*rsqrt(max(d2,0.00000001));
        float nl=saturate(dot(n,l));
        if(nl>0&&nv>0){
            float3 h=normalize(v+l);float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
            float denom=nh*nh*(a2-1)+1;
            float d=a2/max(pi*denom*denom,0.00000001);
            float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));
            float3 f0=lerp(float3(0.04,0.04,0.04),albedo,metallic);
            float3 fresnel=f0+(1-f0)*pow(1-vh,5);
            float3 spec=d*g*fresnel/max(4*nv*nl,0.00000001);
            float3 diffuse=(1-metallic)*(1-fresnel)*albedo/pi;
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
#if LIVE_GBUFFER
            float3 incoming=radiance*nl*cutoff*cone*visibility(worldpos,geometric,l,i);
            result+=(diffuse+spec)*incoming;
            direct_diffuse+=diffuse*incoming;
#else
            result+=(diffuse+spec)*radiance*nl*cutoff*cone*visibility(worldpos,geometric,l,i);
#endif
        }
    }
    // AO belongs to indirect light. There is no verified indirect term yet.
#if LIVE_GBUFFER
    Output o;o.color=float4(min(result,65504),1);
    float3 relative=(worldpos-camera_scale.xyz)*camera_scale.w;
    // The first bounce uses direct diffuse only. The qualified bedroom
    // preview selects the stone walls and the space beyond the doorway as
    // sources; source masks allow independent tests of the two contributions.
    o.position=all(abs(relative)<60000)?float4(relative,(color.a==-3||color.a==-5||color.a==-9||color.a==-11)?2:1):0;
    bool wall_source=color.a==-10 || (color.a<0 && color.a>-2.5);
    // Include the back wall and target visible through the doorway; limiting
    // the source volume to the threshold leaves only floor and crate tops.
    bool door_source=post_process.z>0 && worldpos.x>3000 && worldpos.x<3360 &&
                     worldpos.y>1375 && worldpos.y<1625 && worldpos.z>260 && worldpos.z<550;
    uint source_mask=(uint)max(post_process.z-1,0);
    bool wall_selected=wall_source && (post_process.z<0.5 || (source_mask&1)!=0);
    bool door_selected=door_source && (source_mask&2)!=0;
    o.normal=float4(geometric,1);
    o.diffuse=float4(min(direct_diffuse,65504),door_selected?2:wall_selected?1:0);
    o.albedo=float4(albedo,ao);return o;
#else
    return float4(min(result,65504),1);
#endif
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
cbuffer Material:register(b1){float4 glow_rect;}
cbuffer Lighting:register(b3){float4 camera_scale;float4 options;
    float4 position_range[16];float4 color_intensity[16];float4 direction_outer[16];float4 light_params[16];
    row_major float4x4 gi_view;row_major float4x4 gi_projection;float4 post_process;};
#if LIVE_GI
Texture2D positions:register(t6);Texture2D normals:register(t7);
Texture2D diffuse_light:register(t8);Texture2D surface_albedo:register(t9);
// A single bounded screen-space bounce. Only directly illuminated diffuse
// radiance is gathered, so ambient and previous bounce are never fed back.
float3 indirect_light(int2 pixel){
    const float pi=3.14159265359;uint width,height;positions.GetDimensions(width,height);
    int2 size=int2(width,height);float4 p=positions.Load(int3(pixel,0));
    if(p.w<1.5)return 0;
    float3 n=normals.Load(int3(pixel,0)).xyz;float4 receiver=surface_albedo.Load(int3(pixel,0));
    float radius=clamp(min(size.x,size.y)*0.3334,8,256);float3 sum=0;
    [loop] for(int i=0;i<64;++i){
        float r=sqrt((i+0.5)/64.0)*radius,angle=i*2.39996323;
        int2 qpixel=pixel+int2(round(float2(cos(angle),sin(angle))*r));
        if(any(qpixel<0)||any(qpixel>=size-1))continue;
        float4 q=positions.Load(int3(qpixel,0)),qx=positions.Load(int3(qpixel+int2(1,0),0)),
               qy=positions.Load(int3(qpixel+int2(0,1),0));
        if(q.w<0.5||qx.w<0.5||qy.w<0.5)continue;
        float3 dx=qx.xyz-q.xyz,dy=qy.xyz-q.xyz;
        if(dot(dx,dx)>0.25||dot(dy,dy)>0.25)continue;
        float3 delta=q.xyz-p.xyz;float d2=dot(delta,delta);
        if(d2<0.0025||d2>9)continue;
        float3 direction=delta*rsqrt(d2),qn=normals.Load(int3(qpixel,0)).xyz;
        if(dot(qn,normals.Load(int3(qpixel+int2(1,0),0)).xyz)<0.9||
           dot(qn,normals.Load(int3(qpixel+int2(0,1),0)).xyz)<0.9)continue;
        float receiver_cos=saturate(dot(n,direction)),source_cos=saturate(dot(qn,-direction));
        if(receiver_cos<=0||source_cos<=0)continue;
        float zp=mul(float4(p.xyz,0),gi_view).z,zq=mul(float4(q.xyz,0),gi_view).z;
        bool blocked=false;
        if(zp>0.001&&zq>0.001){
            [unroll] for(int j=1;j<=2;++j){
                float t=j/3.0;int2 middle=int2(round(lerp(float2(pixel),float2(qpixel),t)));
                float4 hit=positions.Load(int3(middle,0));
                float expected=abs(gi_projection[2][3])>0.00001?1/lerp(1/zp,1/zq,t):lerp(zp,zq,t);
                float actual=mul(float4(hit.xyz,0),gi_view).z;
                if(hit.w>0.5&&actual>0&&actual+0.025<expected)blocked=true;
            }
        }
        if(blocked)continue;
        float area=length(cross(dx,dy))*pi*radius*radius/64;
        float form=min(area/d2,1)*receiver_cos*source_cos;
        float4 source=diffuse_light.Load(int3(qpixel,0));
        // The adjacent room is mostly outside the visible selection. Its
        // visible sources receive an additional artistic compensation.
        if(source.a>0.5)sum+=source.rgb*form*(source.a>1.5 ? 4 : 1);
    }
    return min(sum*receiver.rgb*receiver.a/pi*max(post_process.y,1),65504);
}
#endif
float4 main(float2 uv:TEXCOORD0,float4 position:SV_Position):SV_Target{
    float3 x=max(hdr.Sample(samp,uv).rgb,0);
#if LIVE_GI
    x+=indirect_light(int2(position.xy));
#endif
    if(position.x>=glow_rect.x && position.y>=glow_rect.y &&
       position.x<glow_rect.z && position.y<glow_rect.w){
        uint w,h;hdr.GetDimensions(w,h);float2 texel=1.0/float2(w,h);
        float3 glow=0;float weight=0;
        [unroll] for(int j=-2;j<=2;j++)[unroll] for(int i=-2;i<=2;i++){
            float k=exp(-float(i*i+j*j)*0.25);
            float2 q=clamp(uv+float2(i,j)*texel,texel*0.5,1-texel*0.5);
            float4 s=hdr.SampleLevel(samp,q,0);
            glow+=s.rgb*saturate(s.a-1)*k;weight+=k;
        }
        x+=glow/weight*0.4;
    }
    x*=post_process.x>0 ? post_process.x : 1;
    x=saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14));
    x=lerp(12.92*x,1.055*pow(x,1/2.4)-0.055,step(0.0031308,x));
    return float4(x,1);
})";
} // namespace t3render::live::shaders
