#include "../ir/scene_live.hpp"
#include "../src/scene_live_flat.hpp"
#include "../src/scene_live_lighting.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

using namespace t3render::live;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return false; } } while (0)

namespace {
const std::string hash_a(64, 'a'), hash_b(64, 'b');
const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

Resource resource(const std::string& id, ResourceKind kind) {
    Resource r; r.id = id; r.name = id; r.kind = kind; r.source_sha256 = hash_b;
    r.geometry.positions = {{0,0,0}, {1,0,0}, {0,1,0}, {2,0,0}, {3,0,0}, {2,1,0}};
    r.geometry.indices = {0,1,2,3,4,5};
    r.geometry.sections = {{"one", 0, 3, "mat:red"}, {"two", 3, 3, "mat:blue"}};
    return r;
}
FrameInput frame(const Epochs e, std::uint64_t id) {
    FrameInput f; f.map_epoch=e.map; f.device_epoch=e.device; f.frame_id=id;
    f.synchronized=true; f.render_pass=3; f.stage=1;
    f.provenance.origin=FrameOrigin::Synthetic; f.provenance.session_id="fixture-session";
    f.conventions.length_unit=LengthUnit::T3Unit; f.conventions.axes=Axes::T3Xyz;
    f.conventions.up_axis=UpAxis::Z;
    f.camera.view=identity; f.camera.projection=identity; f.camera.width=640; f.camera.height=480;
    MeshSelection mesh; mesh.instance_id="actor:bed"; mesh.resource_id="mesh:bed"; mesh.generation=1;
    mesh.binding=BindingEvidence::Verified; mesh.transform=identity; mesh.sections={0,1};
    BspSelection bsp; bsp.resource_id="bsp:wall"; bsp.binding=BindingEvidence::Candidate; bsp.sections={0};
    f.meshes.push_back(mesh); f.bsp.push_back(bsp);
    return f;
}
bool setup(SceneResidency& scene) {
    CHECK(scene.BeginMap(0, "bedroom", hash_a));
    const auto epoch = scene.epochs().map;
    CHECK(scene.AddMaterial(epoch, {"mat:red", "red"}));
    CHECK(scene.AddMaterial(epoch, {"mat:blue", "blue"}));
    CHECK(scene.AddResource(epoch, resource("mesh:bed", ResourceKind::StaticMesh)));
    CHECK(scene.AddResource(epoch, resource("mesh:chair", ResourceKind::StaticMesh)));
    CHECK(scene.AddResource(epoch, resource("bsp:wall", ResourceKind::BspBlock)));
    return true;
}

bool updates_and_flat_adapter() {
    SceneResidency scene; CHECK(setup(scene));
    auto first = frame(scene.epochs(), 1);
    first.camera.view[12] = 8;
    first.meshes[0].transform[12] = 10;
    CHECK(scene.Publish(first));
    auto old = scene.current();
    CHECK(old && old->frame_id == 1 && old->meshes.size() == 1 && old->bsp.size() == 1);
    CHECK(old->bsp[0].selection.binding == BindingEvidence::Candidate);
    CHECK(old->materials.at("mat:red").name == "red");
    t3render::scene::Frame flat;
    CHECK(BuildFlatFrame(*old, flat));
    ResidentFlatFrame resident;
    CHECK(BuildResidentFlatFrame(*old,resident));
    CHECK(resident.draws.size()==3 && resident.draws[0].resource==old->meshes[0].resource &&
          resident.draws[0].transform[12]==10 && resident.camera.view[12]==8 &&
          resident.draws[0].color[0]==flat.draws[0].ambient_color[0]);
    auto high=std::make_shared<const Resource>(resource("mesh:bed_high",ResourceKind::StaticMesh));
    MeshOverride replacement{"bedroom",hash_a,"actor:bed",old->meshes[0].resource,high};
    CHECK(BuildResidentFlatFrame(*old,resident,&replacement));
    CHECK(resident.override_instances==1 && resident.draws.size()==3 && resident.draws[0].resource==high &&
          resident.draws[0].transform==old->meshes[0].selection.transform && old->meshes[0].resource!=high);
    auto partial=*old;partial.meshes[0].selection.sections={1};
    CHECK(BuildResidentFlatFrame(partial,resident,&replacement) && resident.override_instances==1 && resident.draws.size()==3);
    auto foreign=replacement;foreign.source=std::make_shared<const Resource>(*replacement.source);
    CHECK(!BuildResidentFlatFrame(*old,resident,&foreign));
    CHECK(resident.override_instances==1); // failure does not mutate output
    foreign=replacement;foreign.instance_id="actor:other";
    CHECK(BuildResidentFlatFrame(*old,resident,&foreign) && resident.override_instances==0 &&
          resident.draws[0].resource==old->meshes[0].resource);
    foreign=replacement;foreign.map="other";
    CHECK(BuildResidentFlatFrame(*old,resident,&foreign) && resident.override_instances==0);
    foreign=replacement;foreign.map_sha256=hash_b;
    CHECK(BuildResidentFlatFrame(*old,resident,&foreign) && resident.override_instances==0 &&
          resident.draws[0].resource==old->meshes[0].resource);
    CHECK(flat.view[12] == 8 && flat.draws.size() == 3 && flat.vertices.size() == 9);
    CHECK(flat.vertices[0].x == 10 && flat.vertices[1].x == 11 && flat.vertices[6].x == 0);
    CHECK(flat.draws[0].ambient_color[0] != flat.draws[1].ambient_color[0] ||
          flat.draws[0].ambient_color[1] != flat.draws[1].ambient_color[1] ||
          flat.draws[0].ambient_color[2] != flat.draws[1].ambient_color[2]);
    auto second = frame(scene.epochs(), 2);
    second.camera.view[12]=12; second.meshes.clear(); second.bsp[0].sections={1};
    CHECK(scene.Publish(second));
    CHECK(scene.current()->meshes.empty() && scene.current()->bsp[0].selection.sections[0] == 1);
    CHECK(BuildFlatFrame(*scene.current(), flat));
    CHECK(flat.draws.size() == 1 && flat.vertices[0].x == 2 && flat.view[12] == 12);
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));
    CHECK(resident.draws.size()==1 && resident.draws[0].section==1 && resident.camera.view[12]==12);
    CHECK(BuildResidentFlatFrame(*scene.current(),resident,&replacement) && resident.override_instances==0 && resident.draws.size()==1);
    auto third = frame(scene.epochs(), 3);
    third.meshes[0].transform[12]=20; third.meshes[0].sections={1};
    CHECK(scene.Publish(third));
    CHECK(old->meshes[0].selection.transform[12] == 10); // old readers keep consistent values
    CHECK(old->meshes[0].resource == scene.current()->meshes[0].resource); // immutable cache survives
    CHECK(BuildFlatFrame(*scene.current(), flat));
    CHECK(flat.draws.size() == 2 && flat.vertices[0].x == 22);
    auto malformed = *scene.current();
    malformed.meshes[0].selection.transform[3]=0.5f;
    CHECK(!BuildFlatFrame(malformed, flat));
    CHECK(flat.vertices[0].x == 22); // failed conversion leaves output unchanged
    CHECK(!BuildResidentFlatFrame(malformed,resident));
    CHECK(resident.draws.size()==1); // failure leaves prior output intact
    malformed = *scene.current(); malformed.conventions.clip_depth=ClipDepth::MinusOneToOne;
    CHECK(!BuildFlatFrame(malformed, flat));
    third.camera.view[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!scene.Publish(third));
    CHECK(scene.current()->frame_id == 3);
    // Reloading even the same map retires its resources; an old override must
    // never bind to a freshly registered object that happens to share its ID.
    CHECK(scene.BeginMap(scene.epochs().map,"bedroom",hash_a));
    const auto epoch=scene.epochs();
    CHECK(scene.AddMaterial(epoch.map,{"mat:red","red"}));
    CHECK(scene.AddMaterial(epoch.map,{"mat:blue","blue"}));
    CHECK(scene.AddResource(epoch.map,resource("mesh:bed",ResourceKind::StaticMesh)));
    CHECK(scene.AddResource(epoch.map,resource("bsp:wall",ResourceKind::BspBlock)));
    CHECK(scene.Publish(frame(epoch,1)));
    CHECK(scene.current()->meshes[0].resource!=replacement.source);
    CHECK(!BuildResidentFlatFrame(*scene.current(),resident,&replacement));
    return true;
}

bool bedroom_material_selection() {
    const std::string map="SouthQuarter1_int1";
    const auto bed_id="mesh:"+map+":"+hash_a+":"+hash_b+":innbed1a1";
    const auto actor="actor:"+map+":"+hash_a+":StaticMeshActor__1520";
    SceneResidency scene;CHECK(scene.BeginMap(0,map,hash_a));const auto epoch=scene.epochs();
    CHECK(scene.AddMaterial(epoch.map,{"mat:red","red"}));CHECK(scene.AddMaterial(epoch.map,{"mat:blue","blue"}));
    CHECK(scene.AddMaterial(epoch.map,{"mat:floor","HSEfloorB_D"}));
    CHECK(scene.AddMaterial(epoch.map,{"mat:wall","STMwall512D_D"}));
    auto bed=resource(bed_id,ResourceKind::StaticMesh);bed.name="INNbed1A1";
    CHECK(scene.AddResource(epoch.map,std::move(bed)));
    Resource bsp;bsp.id="bsp:bedroom";bsp.name="bedroom";bsp.source_sha256=hash_a;bsp.kind=ResourceKind::BspBlock;
    // Coordinates in the Godot bed-local frame: floor, ceiling, outside,
    // and vertical wall. Convert to T3 Z-up and translate by the anchor.
    const std::array<Vec3,12> points{{{0,0,0},{1,0,0},{0,0,1},
                                      {0,0,0},{0,0,1},{1,0,0},
                                      {7,0,0},{8,0,0},{7,0,1},
                                      {1,0,0},{1,2,0},{1,0,1}}};
    const float scale=0.01905f;
    for (const auto& p:points) bsp.geometry.positions.push_back({p[0]/scale+10,p[2]/scale+20,p[1]/scale+30});
    for (std::uint32_t i=0;i<points.size();++i) bsp.geometry.indices.push_back(i);
    bsp.geometry.sections={{"floor",0,9,"mat:floor"},{"wall",9,3,"mat:wall"}};
    CHECK(scene.AddResource(epoch.map,std::move(bsp)));
    auto input=frame(epoch,1);input.conventions.meters_per_unit=scale;
    input.meshes[0].instance_id=actor;input.meshes[0].resource_id=bed_id;
    input.meshes[0].sections={0};input.meshes[0].transform[12]=10;
    input.meshes[0].transform[13]=20;input.meshes[0].transform[14]=30;
    input.bsp[0].resource_id="bsp:bedroom";input.bsp[0].sections={0,1};
    CHECK(scene.Publish(input));auto current=scene.current();ResidentFlatFrame flat;
    CHECK(BuildResidentFlatFrame(*current,flat,nullptr,false) && flat.draws.size()==3 && !flat.bedroom_material_runs);
    CHECK(BuildResidentFlatFrame(*current,flat,nullptr,true));
    CHECK(flat.bedroom_material_runs==2 && flat.draws.size()==4);
    CHECK(flat.draws[1].first_index==0 && flat.draws[1].index_count==3 &&
          flat.draws[1].material_id=="remaster:material:wood_floor_v2");
    CHECK(flat.draws[2].first_index==3 && flat.draws[2].index_count==6 && flat.draws[2].material_id=="mat:floor");
    CHECK(flat.draws[3].first_index==9 && flat.draws[3].index_count==3 &&
          flat.draws[3].material_id=="remaster:material:stone_wall_v2");
    CHECK(current->bsp[0].resource->geometry.sections[0].index_count==9 && current->materials.size()==4);

    // Inverse transform handles nonuniform scale, rotation and translation;
    // the indexed triangles remain in their original order.
    auto rotated=*current;const Matrix transform{0,2,0,0, -3,0,0,0, 0,0,4,0, 11,-7,9,1};
    rotated.meshes[0].selection.transform=transform;
    auto shifted=std::make_shared<Resource>(*current->bsp[0].resource);
    for (auto& p:shifted->geometry.positions) {
        const Vec3 local{p[0]-10,p[1]-20,p[2]-30};
        p={local[0]*transform[0]+local[1]*transform[4]+local[2]*transform[8]+transform[12],
           local[0]*transform[1]+local[1]*transform[5]+local[2]*transform[9]+transform[13],
           local[0]*transform[2]+local[1]*transform[6]+local[2]*transform[10]+transform[14]};
    }
    rotated.bsp[0].resource=shifted;
    CHECK(BuildResidentFlatFrame(rotated,flat,nullptr,true) && flat.bedroom_material_runs==2 && flat.draws.size()==4);
    CHECK(flat.draws[1].first_index==0 && flat.draws[2].index_count==6 && flat.draws[3].first_index==9);

    auto moved=*current;moved.meshes[0].selection.transform[12]+=1000;
    CHECK(BuildResidentFlatFrame(moved,flat,nullptr,true) && !flat.bedroom_material_runs && flat.draws.size()==3);
    auto wrong=*current;wrong.map_sha256=hash_b;
    CHECK(BuildResidentFlatFrame(wrong,flat,nullptr,true) && !flat.bedroom_material_runs);
    wrong=*current;wrong.meshes[0].selection.transform[0]=0;
    CHECK(BuildResidentFlatFrame(wrong,flat,nullptr,true) && !flat.bedroom_material_runs);
    input.frame_id=2;input.meshes.clear();CHECK(scene.Publish(input));
    CHECK(BuildResidentFlatFrame(*scene.current(),flat,nullptr,true) && !flat.bedroom_material_runs && flat.draws.size()==2);
    return true;
}

VisualLight point_light() {
    VisualLight light;light.id="light:candle";light.generation=1;light.binding=BindingEvidence::Verified;
    light.kind={LightKind::Point,BindingEvidence::Verified};light.enabled={true,BindingEvidence::Verified};
    light.position={Vec3{1,2,3},BindingEvidence::Verified};light.color_linear={Vec3{1,.5f,.25f},BindingEvidence::Verified};
    light.intensity={2,BindingEvidence::Verified};light.influence_radius={100,BindingEvidence::Verified};
    light.source_radius_m={.05f,BindingEvidence::Verified};return light;
}

bool light_lifecycle_and_evidence() {
    SceneResidency scene;CHECK(setup(scene));auto f=frame(scene.epochs(),1);auto light=point_light();
    f.conventions.meters_per_unit=.01905f;
    f.camera.view={0,2,0,0, -3,0,0,0, 0,0,4,0, 11,-7,9,1};
    CHECK(ClassifyVisualLight(light)==LightReadiness::Ready);
    auto incomplete=light;incomplete.color_linear.evidence=BindingEvidence::Candidate;
    CHECK(ClassifyVisualLight(incomplete)==LightReadiness::Incomplete);
    auto off=light;off.enabled={false,BindingEvidence::Verified};off.intensity={};
    CHECK(ClassifyVisualLight(off)==LightReadiness::Disabled);
    off.enabled.evidence=BindingEvidence::Candidate;CHECK(ClassifyVisualLight(off)==LightReadiness::Incomplete);
    auto spot=light;spot.kind={LightKind::Spot,BindingEvidence::Verified};
    CHECK(ClassifyVisualLight(spot)==LightReadiness::Incomplete);
    spot.direction={Vec3{0,0,-1},BindingEvidence::Verified};spot.cos_inner={.9f,BindingEvidence::Verified};
    spot.cos_outer={.7f,BindingEvidence::Verified};CHECK(ClassifyVisualLight(spot)==LightReadiness::Ready);

    f.lights={light};CHECK(!scene.Publish(f)); // unknown coverage cannot carry observations
    f.light_coverage=LightCoverage::Partial;CHECK(scene.Publish(f));auto first=scene.current();
    CHECK(first->light_coverage==LightCoverage::Partial && first->lights.size()==1 && first->lights[0]==light);
    VerifiedLighting lighting;lighting.frame_id=999;
    CHECK(BuildVerifiedLighting(*first,lighting)==LightingResult::UnknownCoverage && lighting.frame_id==999);
    auto complete=*first;complete.light_coverage=LightCoverage::Complete;
    spot.id="light:spot";complete.lights.push_back(spot);
    CHECK(BuildVerifiedLighting(complete,lighting)==LightingResult::Ready && lighting.frame_id==1 && lighting.lights.size()==2);
    CHECK(LightingMatchesSnapshot(complete,lighting));
    auto next_snapshot=complete;++next_snapshot.frame_id;CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    next_snapshot=complete;++next_snapshot.continuity;CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    next_snapshot=complete;next_snapshot.provenance.session_id="other";CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    next_snapshot=complete;next_snapshot.camera.view[12]+=1;CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    next_snapshot=complete;next_snapshot.light_coverage=LightCoverage::Partial;CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    next_snapshot=complete;next_snapshot.lights[0].enabled.value=false;CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    next_snapshot=complete;next_snapshot.conventions.axes=Axes::Unknown;CHECK(!LightingMatchesSnapshot(next_snapshot,lighting));
    CHECK(std::fabs(lighting.camera_position[0]-3.5f)<1e-5f &&
          std::fabs(lighting.camera_position[1]-11.0f/3)<1e-5f && std::fabs(lighting.camera_position[2]+2.25f)<1e-5f);
    CHECK(std::fabs(lighting.lights[0].influence_radius_m-1.905f)<1e-5f && lighting.lights[0].generation==1 &&
          lighting.lights[1].kind==LightKind::Spot);
    auto invalid=complete;invalid.lights[0].color_linear.evidence=BindingEvidence::Candidate;
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::Incomplete && lighting.lights.size()==2);
    invalid=complete;invalid.lights[0]=off;invalid.lights[0].enabled.evidence=BindingEvidence::Verified;
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::Ready && lighting.lights.size()==1);
    invalid=complete;invalid.lights.resize(17,light);
    for (std::size_t i=0;i<invalid.lights.size();++i) invalid.lights[i].id="light:"+std::to_string(i);
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::TooManyLights && lighting.lights.size()==1);
    invalid=complete;invalid.lights.push_back(invalid.lights[0]);
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::Incomplete);
    invalid=complete;invalid.camera.view[0]=invalid.camera.view[1]=invalid.camera.view[2]=0;
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::InvalidCamera);
    invalid=complete;invalid.conventions.meters_per_unit.reset();
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::InvalidUnits);
    invalid=complete;invalid.provenance.origin=FrameOrigin::LiveProcess;invalid.provenance.capture_sha256=hash_a;
    CHECK(BuildVerifiedLighting(invalid,lighting)==LightingResult::InvalidProvenance);
    f.frame_id=2;f.lights[0].color_linear.evidence=BindingEvidence::Candidate;CHECK(scene.Publish(f));
    CHECK(ClassifyVisualLight(scene.current()->lights[0])==LightReadiness::Incomplete);
    CHECK(first->lights[0].color_linear.evidence==BindingEvidence::Verified);
    auto bad=f;bad.frame_id=3;bad.lights[0].generation=2;bad.lights.push_back(bad.lights[0]);CHECK(!scene.Publish(bad));
    bad=f;bad.frame_id=3;bad.lights[0].enabled.evidence=BindingEvidence::Unknown;CHECK(!scene.Publish(bad));
    bad=f;bad.frame_id=3;bad.lights[0].position.value=Vec3{std::numeric_limits<float>::quiet_NaN(),0,0};CHECK(!scene.Publish(bad));
    bad=f;bad.frame_id=3;bad.lights[0].intensity.value=-1;CHECK(!scene.Publish(bad));
    bad=f;bad.frame_id=3;bad.lights[0].direction={Vec3{0,0,2},BindingEvidence::Verified};CHECK(!scene.Publish(bad));
    bad=f;bad.frame_id=3;bad.lights[0]=spot;bad.lights[0].cos_outer.value=.95f;CHECK(!scene.Publish(bad));
    CHECK(scene.current()->frame_id==2);
    auto unchanged=f;unchanged.frame_id=3;CHECK(scene.Publish(unchanged)); // rejected generation was not committed
    f.frame_id=4;f.lights.clear();f.light_coverage=LightCoverage::Complete;CHECK(scene.Publish(f));
    CHECK(scene.current()->lights.empty() && scene.current()->light_coverage==LightCoverage::Complete);
    f.frame_id=5;light.generation=2;f.lights={light};CHECK(scene.Publish(f));
    f.frame_id=6;f.lights[0].generation=1;CHECK(!scene.Publish(f));
    f.lights.clear();f.light_coverage=LightCoverage::Unknown;CHECK(scene.Publish(f));
    CHECK(scene.current()->lights.empty() && scene.current()->light_coverage==LightCoverage::Unknown);
    CHECK(scene.BeginMap(scene.epochs().map,"bedroom",hash_a));
    auto reset=frame(scene.epochs(),1);reset.meshes.clear();reset.bsp.clear();
    reset.light_coverage=LightCoverage::Partial;reset.lights={point_light()};CHECK(scene.Publish(reset));
    return true;
}

bool atomic_validation_and_generations() {
    SceneResidency scene; CHECK(setup(scene));
    auto f = frame(scene.epochs(), 10); CHECK(scene.Publish(f));
    auto wrong_source = frame(scene.epochs(), 11);
    wrong_source.provenance.origin=FrameOrigin::LiveProcess; CHECK(!scene.Publish(wrong_source));
    wrong_source.provenance.origin=FrameOrigin::CapturedReplay; CHECK(!scene.Publish(wrong_source));
    wrong_source.provenance.capture_sha256=hash_b; CHECK(!scene.Publish(wrong_source));
    auto wrong_conventions = frame(scene.epochs(), 11);
    wrong_conventions.conventions.clip_depth=ClipDepth::ZeroToOne; CHECK(!scene.Publish(wrong_conventions));
    wrong_conventions.conventions.meters_per_unit=std::numeric_limits<float>::quiet_NaN();
    CHECK(!scene.Publish(wrong_conventions));
    auto bad = frame(scene.epochs(), 11);
    bad.meshes[0].generation=2; bad.meshes[0].resource_id="mesh:chair"; bad.bsp[0].resource_id="bsp:missing";
    CHECK(!scene.Publish(bad));
    CHECK(scene.current()->frame_id == 10);
    auto still_old = frame(scene.epochs(), 11); CHECK(scene.Publish(still_old)); // rejected update did not change identity
    bad = frame(scene.epochs(), 12); bad.meshes[0].resource_id="mesh:chair";
    CHECK(!scene.Publish(bad)); // resource cannot silently change within one generation
    bad.meshes[0].generation=2; CHECK(scene.Publish(bad));
    auto stale = frame(scene.epochs(), 13); CHECK(!scene.Publish(stale));
    stale.meshes[0].generation=2; stale.meshes[0].resource_id="mesh:chair";
    stale.meshes[0].binding=BindingEvidence::Unknown; CHECK(!scene.Publish(stale));
    stale.meshes[0].binding=BindingEvidence::Candidate; stale.meshes[0].sections={0,0}; CHECK(!scene.Publish(stale));
    stale.meshes[0].sections={0}; stale.meshes[0].material_ids={"mat:red"}; CHECK(!scene.Publish(stale));
    stale.meshes[0].material_ids={"mat:red", std::nullopt}; CHECK(scene.Publish(stale));
    CHECK(scene.current()->meshes[0].selection.binding == BindingEvidence::Candidate);
    CHECK(!scene.Publish(stale)); // no stale or duplicate frame IDs
    CHECK(!scene.AddResource(scene.epochs().map, resource("0x123abc", ResourceKind::StaticMesh)));
    auto altered=resource("mesh:chair", ResourceKind::StaticMesh); altered.geometry.positions[0][0]=99;
    CHECK(!scene.AddResource(scene.epochs().map, altered));
    CHECK(scene.AddResource(scene.epochs().map, resource("mesh:chair", ResourceKind::StaticMesh)));
    return true;
}

bool reset_and_map_change() {
    SceneResidency scene; CHECK(setup(scene));
    auto before=frame(scene.epochs(), 30); CHECK(scene.Publish(before));
    auto old=scene.current();
    const auto first=scene.BeginReset(); CHECK(first && !scene.current() && !scene.epochs().ready);
    CHECK(!scene.Publish(before)); CHECK(!scene.BeginMap(scene.epochs().map, "next", hash_b));
    CHECK(scene.EndReset(first, false)); CHECK(!scene.epochs().ready);
    auto retry=frame(scene.epochs(), 31); CHECK(!scene.Publish(retry));
    const auto second=scene.BeginReset(); CHECK(second && second != first);
    CHECK(!scene.EndReset(first, true)); CHECK(scene.EndReset(second, true));
    retry.device_epoch=scene.epochs().device; CHECK(scene.Publish(retry));
    CHECK(scene.current()->meshes[0].resource == old->meshes[0].resource);
    auto previous=scene.epochs(); CHECK(scene.BeginMap(previous.map, "bedroom", hash_a));
    CHECK(!scene.current()); CHECK(!scene.BeginMap(previous.map, "stale", hash_b));
    CHECK(!scene.AddMaterial(previous.map, {"mat:bad", "bad"}));
    CHECK(!scene.AddResource(previous.map, resource("mesh:bed", ResourceKind::StaticMesh)));
    CHECK(!scene.Publish(retry));
    CHECK(old->map == "bedroom" && old->meshes[0].resource->geometry.positions[1][0] == 1);
    auto new_epoch=scene.epochs();
    CHECK(new_epoch.map != previous.map && new_epoch.device != previous.device);
    CHECK(scene.AddMaterial(new_epoch.map, {"mat:red", "red"}));
    CHECK(scene.AddMaterial(new_epoch.map, {"mat:blue", "blue"}));
    CHECK(scene.AddResource(new_epoch.map, resource("mesh:bed", ResourceKind::StaticMesh)));
    CHECK(scene.AddResource(new_epoch.map, resource("bsp:wall", ResourceKind::BspBlock)));
    auto fresh=frame(new_epoch, 1); CHECK(scene.Publish(fresh));
    CHECK(scene.current()->meshes[0].resource != old->meshes[0].resource);
    return true;
}

bool concurrent_readers() {
    SceneResidency scene; CHECK(setup(scene));
    auto initial=frame(scene.epochs(), 1); CHECK(scene.Publish(initial));
    std::atomic<bool> done{false}, good{true};
    std::thread reader([&] {
        while (!done.load()) {
            auto snap=scene.current();
            if (!snap || snap->meshes.size()!=1 || snap->camera.view[12] != snap->meshes[0].selection.transform[12])
                good.store(false);
        }
    });
    for (std::uint64_t i=2; i<1002; ++i) {
        auto next=frame(scene.epochs(), i);
        next.camera.view[12]=static_cast<float>(i);
        next.meshes[0].transform[12]=static_cast<float>(i);
        if (!scene.Publish(next)) good.store(false);
    }
    done.store(true); reader.join();
    CHECK(good.load());
    return true;
}

struct Gate { std::atomic<bool> entered{false}, release{false}; unsigned calls=0; };
bool gated_submit(void* context) noexcept {
    auto& gate=*static_cast<Gate*>(context); ++gate.calls; gate.entered.store(true);
    while (!gate.release.load()) std::this_thread::yield();
    return true;
}
bool counted_submit(void* context) noexcept { ++*static_cast<unsigned*>(context); return true; }
bool submission_epoch_gate() {
    SceneResidency scene; CHECK(setup(scene)); CHECK(scene.Publish(frame(scene.epochs(),1)));
    auto first=scene.current(); unsigned calls=0;
    CHECK(scene.SubmitIfCurrent(first,counted_submit,&calls)); CHECK(calls==1);
    CHECK(scene.Publish(frame(scene.epochs(),2)));
    CHECK(!scene.SubmitIfCurrent(first,counted_submit,&calls) && calls==1);
    auto current=scene.current(); Gate gate; std::atomic<bool> reset_started{false}, reset_done{false};
    CHECK(DisplayContinuityMatches(*first,*current) && !DisplayContinuityMatches(*current,*first));
    std::uint64_t reset_ticket=0; bool submitted=false;
    std::thread consumer([&] { submitted=scene.SubmitIfCurrent(current,gated_submit,&gate); });
    while (!gate.entered.load()) std::this_thread::yield();
    std::thread reset([&] { reset_started.store(true); reset_ticket=scene.BeginReset(); reset_done.store(true); });
    while (!reset_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool raced=reset_done.load(); gate.release.store(true); consumer.join(); reset.join();
    CHECK(!raced && submitted && gate.calls==1 && reset_ticket && reset_done.load());
    CHECK(!scene.SubmitIfCurrent(current,counted_submit,&calls) && calls==1);
    CHECK(scene.EndReset(reset_ticket,true));
    auto next=frame(scene.epochs(),3); CHECK(scene.Publish(next));
    CHECK(!DisplayContinuityMatches(*current,*scene.current()));
    CHECK(!scene.SubmitIfCurrent(current,counted_submit,&calls));
    CHECK(scene.SubmitIfCurrent(scene.current(),counted_submit,&calls) && calls==2);
    auto before_loss=scene.current();
    CHECK(scene.DiscardFrame(before_loss->map_epoch,before_loss->device_epoch,before_loss->frame_id));
    CHECK(scene.Publish(frame(scene.epochs(),4)));
    CHECK(!DisplayContinuityMatches(*before_loss,*scene.current()));
    const auto map=scene.epochs().map; CHECK(scene.BeginMap(map,"next",hash_b));
    CHECK(!scene.SubmitIfCurrent(current,counted_submit,&calls) && calls==2);
    return true;
}
} // namespace

int main() {
    if (!updates_and_flat_adapter() || !bedroom_material_selection() || !light_lifecycle_and_evidence() || !atomic_validation_and_generations() ||
        !reset_and_map_change() || !concurrent_readers() || !submission_epoch_gate()) return 1;
    std::cout << "scene_live_fixture=ok\n";
}
