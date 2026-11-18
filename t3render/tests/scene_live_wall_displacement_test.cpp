#include "../src/scene_live_wall_displacement.hpp"

#include <cmath>
#include <iostream>

using namespace t3render::live;
#define CHECK(value) do { if (!(value)) {std::cerr<<"line "<<__LINE__<<": " #value "\n";return 1;} } while (0)

int main() {
    const std::string hash="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
    const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Snapshot snapshot;snapshot.map="SouthQuarter1_int1";snapshot.map_sha256=hash;
    snapshot.map_epoch=1;snapshot.device_epoch=1;snapshot.continuity=1;snapshot.frame_id=3;
    snapshot.conventions.length_unit=LengthUnit::T3Unit;snapshot.conventions.axes=Axes::T3Xyz;
    snapshot.conventions.up_axis=UpAxis::Z;snapshot.conventions.meters_per_unit=.01905f;
    auto bed=std::make_shared<Resource>();bed->kind=ResourceKind::StaticMesh;bed->name="innbed1a1";
    bed->source_sha256=std::string(64,'b');bed->id="mesh:"+snapshot.map+":"+hash+":"+bed->source_sha256+":innbed1a1";
    ResidentMesh instance;instance.resource=bed;instance.selection.resource_id=bed->id;
    instance.selection.instance_id="actor:"+snapshot.map+":"+hash+":StaticMeshActor__1520";
    instance.selection.generation=1;instance.selection.binding=BindingEvidence::Candidate;
    instance.selection.sections={0};instance.selection.transform=identity;
    instance.selection.transform[12]=3398;instance.selection.transform[13]=1514;instance.selection.transform[14]=272;
    snapshot.meshes.push_back(instance);
    auto source=std::make_shared<Resource>();source->kind=ResourceKind::BspBlock;source->id="source-wall";
    source->source_sha256=hash;source->geometry.positions={{3398,1534,292},{3398,1566,292},{3398,1534,324}};
    source->geometry.normals.assign(3,Vec3{1,0,0});source->geometry.uv0={{0,0},{1,0},{0,1}};
    source->geometry.indices={0,1,2};source->geometry.sections={{"wall",0,3,"mat:wall"}};
    ResidentFlatDraw draw;draw.resource=source;draw.section=0;draw.index_count=3;draw.transform=identity;
    draw.material_id="remaster:material:stone_wall_v2";draw.color={1,1,1,1};draw.wall_relief=2;
    ResidentFlatFrame initial;initial.map=snapshot.map;initial.map_sha256=hash;initial.map_epoch=1;
    initial.device_epoch=1;initial.continuity=1;initial.frame_id=3;initial.draws={draw};
    auto material=std::make_shared<BedroomMaterialLibrary>();material->wall_height_source_sha256=std::string(64,'c');
    material->wall_height=t3render::scene::Texture{};
    material->wall_height->mips.push_back({2,2,std::vector<std::uint8_t>{0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255}});
    WallDisplacementCache cache;WallDisplacementStats stats;auto frame=initial;
    CHECK(cache.Apply(snapshot,frame,material,stats));
    CHECK(stats.source_triangles==1 && stats.displaced_triangles==576);
    CHECK(frame.draws[0].wall_displaced && frame.draws[0].wall_relief==1 && frame.draws[0].index_count==576*3);
    const auto generated=frame.draws[0].resource;
    CHECK(generated!=source && generated->geometry.positions.size()==325 && source->geometry.indices.size()==3);
    for (const auto& p:generated->geometry.positions) CHECK(std::fabs(p[0]-(3398-.05f/.01905f))<3e-4f);
    for (const auto& n:generated->geometry.normals) CHECK(n[0]>.99f);
    for (auto index:generated->geometry.indices) CHECK(index<generated->geometry.positions.size());
    auto again=initial;CHECK(cache.Apply(snapshot,again,material,stats) && again.draws[0].resource==generated);
    cache.Clear();again=initial;
    CHECK(cache.Apply(snapshot,again,material,stats) && again.draws[0].resource!=generated && again.draws[0].resource->id!=generated->id);
    auto stale=initial;++stale.frame_id;CHECK(!cache.Apply(snapshot,stale,material,stats) && stale.draws[0].resource==source);
    auto white=std::make_shared<BedroomMaterialLibrary>(*material);
    for (std::size_t i=0;i<white->wall_height->mips[0].rgba.size();i+=4) white->wall_height->mips[0].rgba[i]=255;
    auto flat=initial;CHECK(cache.Apply(snapshot,flat,white,stats));
    CHECK(flat.draws[0].resource!=generated);
    for (const auto& p:flat.draws[0].resource->geometry.positions) CHECK(std::fabs(p[0]-3398)<1e-6f);
    auto edge_source=std::make_shared<Resource>(*source);edge_source->id="edge-wall";
    edge_source->geometry.positions={{3398,1408,292},{3398,1440,292},{3398,1408,324}};
    auto edge_frame=initial;edge_frame.draws[0].resource=edge_source;
    CHECK(cache.Apply(snapshot,edge_frame,material,stats));
    CHECK(std::fabs(edge_frame.draws[0].resource->geometry.positions[0][0]-3398)<.001f);

    // The same height-mapped edge can cross separate draw resources. Its
    // duplicated vertices must receive matching, non-flat lighting normals.
    auto adjacent=std::make_shared<Resource>(*source);adjacent->id="adjacent-wall";
    adjacent->geometry.positions={{3398,1566,324},{3398,1534,324},{3398,1566,292}};
    adjacent->geometry.uv0={{1,1},{0,1},{1,0}};
    auto gradient=std::make_shared<BedroomMaterialLibrary>(*material);
    std::vector<std::uint8_t> pixels(8*8*4,255);
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x)
        pixels[(y*8+x)*4]=static_cast<std::uint8_t>((x*31+y*17)%256);
    gradient->wall_height->mips={{8,8,std::move(pixels)}};
    auto seam=initial;auto second=draw;second.resource=adjacent;seam.draws.push_back(second);
    CHECK(cache.Apply(snapshot,seam,gradient,stats));
    const auto& first_geometry=seam.draws[0].resource->geometry;
    const auto& second_geometry=seam.draws[1].resource->geometry;
    const auto vertex=[](unsigned i,unsigned j) {return i*(51-i)/2+j;};
    bool nonflat=false;
    for (unsigned t=0;t<=24;++t) {
        const auto a=vertex(24-t,t),b=vertex(t,24-t);
        const auto& n=first_geometry.normals[a];const auto& other=second_geometry.normals[b];
        const float cosine=n[0]*other[0]+n[1]*other[1]+n[2]*other[2];
        CHECK(cosine>.9999f);
        nonflat=nonflat || std::fabs(n[1])>.01f || std::fabs(n[2])>.01f;
    }
    CHECK(nonflat);
    auto degenerate=std::make_shared<Resource>(*source);degenerate->id="degenerate-uv";
    degenerate->geometry.uv0.assign(3,Vec2{0,0});
    auto fallback=initial;fallback.draws[0].resource=degenerate;
    CHECK(cache.Apply(snapshot,fallback,gradient,stats));
    for (const auto& n:fallback.draws[0].resource->geometry.normals) CHECK(std::isfinite(n[0]) && n[0]>.99f);
    white->wall_height.reset();auto no_height=initial;
    CHECK(!cache.Apply(snapshot,no_height,white,stats) && no_height.draws[0].resource==source);
    std::cout<<"wall_displacement=ok geometry=576 cache=reused stale=rejected flat=unchanged\n";
}
