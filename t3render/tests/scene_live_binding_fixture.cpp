#include "../src/scene_live_binding.hpp"
#include "../src/scene_live_driver.hpp"
#include "../src/scene_live_lighting.hpp"
#ifdef T3RENDER_TEST_LIVE_DX12_DRIVER
#include "../src/scene_live_viewer.hpp"
#endif

#include <cstring>
#include <iostream>
#include <limits>

using namespace t3render::live;
using namespace t3render::scene_bridge;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return false; } } while (0)
namespace {
const std::string map_hash(64,'a'), bundle_hash(64,'b');
const std::string actor="actor:bedroom:"+map_hash+":StaticMeshActor__1520";
const std::string bed_id="mesh:bedroom:"+map_hash+":"+bundle_hash+":innbed1a1";
const std::string bsp_id="bsp:bedroom:"+map_hash+":5:100";
const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
std::uint32_t bits(float f) { std::uint32_t r; std::memcpy(&r,&f,sizeof(r)); return r; }
template<std::size_t N> void text(char (&out)[N], const char* value) {
    const auto length=std::strlen(value); if (length<N) std::memcpy(out,value,length+1);
}
void matrix(std::uint32_t* out) { for (unsigned i=0;i<16;++i) out[i]=bits(identity[i]); }
void bounds(std::uint32_t* out) { const float b[]={0,0,0,1,1,0};for(unsigned i=0;i<6;++i)out[i]=bits(b[i]); }
Resource resource(std::string id, ResourceKind kind) {
    Resource r;r.id=std::move(id);r.name=kind==ResourceKind::StaticMesh?"INNbed1A1":"wall";
    r.kind=kind;r.source_sha256=kind==ResourceKind::StaticMesh?bundle_hash:map_hash;
    r.geometry.positions={{0,0,0},{1,0,0},{0,1,0}};r.geometry.indices={0,1,2};
    r.geometry.sections={{"Default",0,3,"mat:stone"}};
    if (kind==ResourceKind::StaticMesh) r.geometry.sections[0].material_id=std::nullopt;
    return r;
}
BindingCatalog catalog() {
    BindingCatalog c;c.map="bedroom";c.map_sha256=map_hash;c.bed_instance_id=actor;
    c.materials.emplace("mat:stone",Material{"mat:stone","Stone"});
    c.bed=std::make_shared<const Resource>(resource(bed_id,ResourceKind::StaticMesh));
    c.bed_materials={"mat:stone"};c.bsp_catalog_complete=true;
    c.bsp.push_back(std::make_shared<const Resource>(resource(bsp_id,ResourceKind::BspBlock)));return c;
}
ProbeSnapshot probe() {
    ProbeSnapshot p;p.thread_id=17;p.self=0x1000;p.caller=0x109271ae;p.known_caller=true;
    p.pass={0,true};p.pass_stable=true;p.object_table_complete=true;
    p.current_camera.address={0x100,true};p.current_camera.stable=true;
    p.current_camera.derived_matrix_available=true;p.current_camera.projection_available=true;
    matrix(p.current_camera.derived_matrix_bits);matrix(p.current_camera.projection_bits);
    auto& t=p.target;t.entry_available=true;t.object_scan_complete=true;t.list_scan_complete=true;
    t.actors_named_target=1;t.list_occurrences=1;t.outer_count=1;
    t.outer[0].member=t.outer[0].stable=t.outer[0].name_available=true;text(t.outer[0].name,"LOADGAME:bedroom");
    auto& e=t.entry;e.address={0x3000,true};e.vtable={0x10e4e8c0,true};e.type={4,true};
    e.owner_candidate={0x4000,true};e.owner_object_index={2,true};e.owner_fname={(1521u<<16)|7,true};
    e.owner_class={0x7000,true};e.owner_backlink={0x3000,true};e.owner_table_member=e.owner_stable=true;
    e.owner_name_available=e.object_matrix_available=true;text(e.owner_name,"StaticMeshActor");matrix(e.object_matrix_bits);
    auto& cls=t.owner_class;cls.address={0x7000,true};cls.fname={7,true};
    cls.member=cls.stable=cls.name_available=true;text(cls.name,"StaticMeshActor");
    auto& asset=t.asset;asset.status=AssetStatus::Candidate;asset.stable=asset.name_available=true;
    asset.descriptor_match=asset.local_property_found=asset.proxy_live_member=true;
    asset.proxy_vtable_match=asset.resource_vtable_match=true;asset.audit.available=true;
    asset.audit.proxy=0x5000;asset.audit.resource=0x6000;text(asset.name,"innbed1a1");
    auto& node=t.mesh_node;node.pool_scan_complete=node.stable=node.name_available=true;node.occurrences=1;
    node.address={0x8000,true};node.vtable={0x10e49d14,true};node.name_address={0x8100,true};
    node.counts_available=node.bounds_available=node.sections_available=true;
    node.vertex_count=3;node.triangle_count=1;node.index_count=3;node.section_count=node.sections_copied=1;
    text(node.name,"innbed1a1");bounds(node.bounds_bits);
    auto& ms=node.sections[0];ms.name_available=ms.material_name_available=true;text(ms.name,"Default");
    text(ms.material_name,"Stone");ms.triangles=1;ms.vertex_count=3;ms.index_count=3;
    auto& b=p.bsp;b.source={0x9000,true};b.blocks={0x10000,true};b.block_count=8;
    b.source_stable=true;b.type5_seen=b.member_count=b.candidates_copied=1;
    auto& block=b.candidates[0];block.list_index=0;block.source_index=7;block.address={0x10000+7*0x1ac,true};
    block.metadata_available=block.stable=block.sections_available=true;
    block.vertex_count=3;block.triangle_count=1;block.index_count=3;block.section_count=block.sections_copied=1;
    bounds(block.bounds_bits);matrix(block.transform_bits);
    auto& bs=block.sections[0];bs.material_name_available=true;text(bs.material_name,"Stone");
    bs.triangles=1;bs.vertex_count=3;bs.index_count=3;
    p.entries_attempted=1;p.count={1,true};p.array={0x2000,true};
    p.entries[0].list_index=0;p.entries[0].address=block.address;
    p.entries[0].vtable={0x10e4a668,true};p.entries[0].type={5,true};
    return p;
}
ProbeSnapshot without_bed() {
    auto p=probe();auto& witness=p.map_witness;witness.available=witness.list_stable=witness.object_table_complete=true;
    witness.entry=p.target.entry;witness.entry.list_index=1;witness.entry.owner_fname.value=(2u<<16)|7;
    witness.owner_class=p.target.owner_class;witness.owner_outer={0xa000,true};
    witness.outer=p.target.outer[0];witness.outer.address={0xa000,true};witness.outer.fname={8,true};witness.outer.outer={0,true};
    p.entries[1]=witness.entry;p.entries_attempted=2;p.count.value=2;p.target={};return p;
}
SubmissionSample bed_scope() {
    SubmissionSample s;s.valid=true;s.self=0x1000;s.pass=0;s.element=0x3000;s.vtable=0x10e4e8c0;s.type=4;
    matrix(s.matrix);s.owner=0x4000;s.owner_index=2;s.owner_fname=(1521u<<16)|7;s.backlink=0x3000;
    s.mesh_node=0x8000;s.asset_available=true;s.asset_proxy=0x5000;s.asset_resource=0x6000;
    text(s.asset_name,"innbed1a1");return s;
}
SubmissionSample bsp_scope() {
    SubmissionSample s;s.valid=true;s.self=0x1000;s.pass=0;s.element=0x10000+7*0x1ac;s.vtable=0x10e4a668;
    s.type=5;matrix(s.matrix);s.bsp_source=0x9000;s.bsp_blocks=0x10000;s.bsp_index=7;return s;
}
bool matching() {
    auto c=catalog();auto p=probe();auto bed=bed_scope();auto block=bsp_scope();
    t3render::live::ProbeCamera cam;CHECK(ReadProbeCamera(p,17,0,cam) && cam.view==identity);
    CHECK(ScopeCameraMatches(cam,identity,identity));auto moved=identity;moved[12]=1;
    CHECK(!ScopeCameraMatches(cam,moved,identity));
    CHECK(ScopeWorldMatches(bed,identity) && !ScopeWorldMatches(bed,moved));
    MeshSelection mesh;BspSelection bsp;
    CHECK(ResolveBed(c,p,bed,0,1,mesh));
    CHECK(mesh.instance_id==actor && mesh.resource_id==bed_id && mesh.binding==BindingEvidence::Candidate);
    CHECK(mesh.identity_scope==IdentityScope::FullObjectScan);
    auto live_probe=p;live_probe.target.visible_list_only=true;
    live_probe.target.object_scan_complete=false;live_probe.target.actors_named_target=0;
    CHECK(ResolveBed(c,live_probe,bed,0,1,mesh) && mesh.identity_scope==IdentityScope::VisibleList);
    live_probe.target.list_occurrences=2;CHECK(!ResolveBed(c,live_probe,bed,0,1,mesh));
    IndexedDraw draw{true,true,0,1,0,3};CHECK(ScopeDrawMatches(*c.bed,0,draw));
    auto mismatch=draw;mismatch.first_index=1;CHECK(!ScopeDrawMatches(*c.bed,0,mismatch));
    mismatch=draw;mismatch.vertex_count=2;CHECK(!ScopeDrawMatches(*c.bed,0,mismatch));
    mismatch=draw;mismatch.triangle_list=false;CHECK(!ScopeDrawMatches(*c.bed,0,mismatch));
    mismatch=draw;mismatch.indexed=false;CHECK(!ScopeDrawMatches(*c.bed,0,mismatch));
    CHECK(ResolveBsp(c,p,block,0,bsp));
    CHECK(bsp.resource_id==bsp_id && bsp.binding==BindingEvidence::Candidate);
    auto no_bed=without_bed();
    CHECK(ResolveBsp(c,no_bed,block,0,bsp) && bsp.identity_scope==IdentityScope::VisibleList);
    CHECK(!ResolveBed(c,no_bed,bed,0,1,mesh));
    auto no_witness=no_bed;no_witness.map_witness={};CHECK(!ResolveBsp(c,no_witness,block,0,bsp));
    auto wrong_witness=no_bed;text(wrong_witness.map_witness.outer.name,"another");CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.map_witness.entry.owner_backlink.value++;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.map_witness.entry.owner_fname.value=(1521u<<16)|7;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.map_witness.outer.outer.value=1;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.map_witness.outer.fname.value|=1u<<16;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.map_witness.object_table_complete=false;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.entries[1].address.value++;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.entries[2]=wrong_witness.entries[1];wrong_witness.entries[2].list_index=2;
    wrong_witness.entries_attempted=wrong_witness.count.value=3;CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp));
    wrong_witness=no_bed;wrong_witness.target=probe().target;text(wrong_witness.target.outer[0].name,"another");
    CHECK(!ResolveBsp(c,wrong_witness,block,0,bsp)); // a conflicting bed witness cannot be bypassed
    CHECK(!ReadProbeCamera(p,18,0,cam));CHECK(!ReadProbeCamera(p,17,1,cam));
    CHECK(!ResolveBed(c,p,bed,1,1,mesh));CHECK(!ResolveBed(c,p,bed,0,0,mesh));
    CHECK(!ResolveBsp(c,p,block,1,bsp));
    auto changed=bed;changed.owner++;CHECK(!ScopeUnchanged(bed,changed));
    changed=block;changed.bsp_index=6;CHECK(!ScopeUnchanged(block,changed));
    CHECK(ScopeUnchanged(bed,bed) && ScopeUnchanged(block,block));
    auto bad=p;bad.target.asset.proxy_live_member=false;CHECK(!ResolveBed(c,bad,bed,0,1,mesh));
    bad=p;bad.target.entry.owner_fname.value+=1u<<16;CHECK(!ResolveBed(c,bad,bed,0,1,mesh));
    bad=p;bad.target.mesh_node.sections[0].index_count=6;CHECK(!ResolveBed(c,bad,bed,0,1,mesh));
    bad=p;bad.target.mesh_node.bounds_bits[0]=bits(-1);CHECK(!ResolveBed(c,bad,bed,0,1,mesh));
    bad=p;text(bad.target.outer[0].name,"LOADGAME:another");CHECK(!ResolveBed(c,bad,bed,0,1,mesh));
    CHECK(!ResolveBsp(c,bad,block,0,bsp));
    bad=p;bad.current_camera.stable=false;CHECK(!ResolveBed(c,bad,bed,0,1,mesh));
    bad=p;bad.bsp.candidates_truncated=true;CHECK(!ResolveBsp(c,bad,block,0,bsp));
    bad=p;bad.bsp.candidates[0].sections[0].vertex_count=2;CHECK(!ResolveBsp(c,bad,block,0,bsp));
    bad=p;bad.entries[0].type.value=4;CHECK(!ResolveBsp(c,bad,block,0,bsp));
    bad=p;bad.bsp.candidates[0].transform_bits[12]=bits(1);CHECK(!ResolveBsp(c,bad,block,0,bsp));
    auto incomplete=c;incomplete.bsp_catalog_complete=false;CHECK(!ResolveBsp(incomplete,p,block,0,bsp));
    auto ambiguous=c;auto duplicate=resource("bsp:bedroom:"+map_hash+":6:200",ResourceKind::BspBlock);
    ambiguous.bsp.push_back(std::make_shared<const Resource>(duplicate));CHECK(!ResolveBsp(ambiguous,p,block,0,bsp));
    auto missing_skin=c;missing_skin.bed_materials.clear();CHECK(!ResolveBed(missing_skin,p,bed,0,1,mesh));
    auto wrong_actor=c;wrong_actor.bed_instance_id="actor:other";CHECK(!ResolveBed(wrong_actor,p,bed,0,1,mesh));
    CHECK(mesh.instance_id==actor && bsp.resource_id==bsp_id); // failure did not mutate outputs
    return true;
}

bool static_mesh_matching() {
    auto c=catalog();auto p=without_bed();auto before=bed_scope();
    const auto actor_id="actor:bedroom:"+map_hash+":StaticMeshActor__1";
    const auto mesh_id="mesh:bedroom:"+map_hash+":"+bundle_hash+":inncrate1";
    auto geometry=resource(mesh_id,ResourceKind::StaticMesh);geometry.name="INNcrate1";
    auto mesh=std::make_shared<const Resource>(std::move(geometry));
    c.static_mesh_actors.emplace(actor_id,StaticMeshCandidate{actor_id,mesh,{"mat:stone"}});
    before.owner_fname=(2u<<16)|7;before.matrix[12]=bits(3.25f);text(before.asset_name,"inncrate1");
    ProbeStaticMesh observed;observed.list_stable=observed.object_table_complete=true;observed.name_occurrences=1;
    observed.entry=p.map_witness.entry;observed.entry.object_matrix_bits[12]=bits(3.25f);
    observed.owner_outer=p.map_witness.owner_outer;observed.owner_class=p.map_witness.owner_class;
    observed.outer=p.map_witness.outer;observed.asset=probe().target.asset;observed.mesh_node=probe().target.mesh_node;
    text(observed.asset.name,"inncrate1");text(observed.mesh_node.name,"inncrate1");
    MeshSelection result;CHECK(ResolveStaticMesh(c,p,before,observed,0,1,result));
    CHECK(result.instance_id==actor_id && result.resource_id==mesh_id && result.transform[12]==3.25f &&
          result.binding==BindingEvidence::Candidate && result.identity_scope==IdentityScope::VisibleList &&
          result.matched_resource==mesh && result.material_ids==std::vector<std::optional<std::string>>{"mat:stone"});
    auto bad=observed;bad.name_occurrences=2;CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;bad.object_table_complete=false;CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;bad.entry.owner_backlink.value++;CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;bad.owner_outer.value++;CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;text(bad.outer.name,"another");CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;text(bad.asset.name,"another");CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;bad.asset.audit.proxy++;CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;bad.mesh_node.bounds_bits[0]=bits(-1);CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;bad.mesh_node.vtable.value++;CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    bad=observed;text(bad.mesh_node.sections[0].material_name,"Other");CHECK(!ResolveStaticMesh(c,p,before,bad,0,1,result));
    auto duplicate=p;duplicate.entries[2]=duplicate.entries[1];duplicate.entries[2].list_index=2;
    duplicate.entries_attempted=duplicate.count.value=3;CHECK(!ResolveStaticMesh(c,duplicate,before,observed,0,1,result));
    auto changed=before;changed.owner_fname=(3u<<16)|7;CHECK(!ResolveStaticMesh(c,p,changed,observed,0,1,result));
    CHECK(!ResolveStaticMesh(c,p,before,observed,1,1,result));CHECK(!ResolveStaticMesh(c,p,before,observed,0,0,result));
    CHECK(result.instance_id==actor_id && result.transform[12]==3.25f); // rejections leave output untouched
    return true;
}

bool static_mesh_driver_lifecycle() {
    auto c=catalog();auto memory=std::make_shared<SceneResidency>();CHECK(memory->BeginMap(0,c.map,c.map_sha256));
    const auto epoch=memory->epochs();CHECK(memory->AddMaterial(epoch.map,c.materials.at("mat:stone")));
    CHECK(memory->AddResource(epoch.map,*c.bed));CHECK(memory->AddResource(epoch.map,*c.bsp[0]));
    const auto actor_id="actor:bedroom:"+map_hash+":StaticMeshActor__1";
    const auto mesh_id="mesh:bedroom:"+map_hash+":"+bundle_hash+":inncrate1";
    auto mesh=resource(mesh_id,ResourceKind::StaticMesh);mesh.name="INNcrate1";CHECK(memory->AddResource(epoch.map,std::move(mesh)));
    c.bed=memory->FindSceneResource(epoch.map,bed_id);c.bsp[0]=memory->FindSceneResource(epoch.map,bsp_id);
    auto resident=memory->FindSceneResource(epoch.map,mesh_id);CHECK(resident);
    c.static_mesh_actors.emplace(actor_id,StaticMeshCandidate{actor_id,resident,{"mat:stone"}});
    LoadedCatalog loaded;loaded.residency=memory;loaded.binding=c;
    loaded.conventions.length_unit=LengthUnit::T3Unit;loaded.conventions.axes=Axes::T3Xyz;loaded.conventions.up_axis=UpAxis::Z;
    FrameProvenance source;source.origin=FrameOrigin::Synthetic;source.session_id="static-driver-fixture";
    auto driver=SceneDriver::Create(std::move(loaded),source,0,1);CHECK(driver);
    const ThreadToken thread{17,123};auto p=without_bed();auto before=bed_scope();before.owner_fname=(2u<<16)|7;
    text(before.asset_name,"inncrate1");ProbeStaticMesh observed;observed.list_stable=observed.object_table_complete=true;
    observed.name_occurrences=1;observed.entry=p.map_witness.entry;observed.owner_outer=p.map_witness.owner_outer;
    observed.owner_class=p.map_witness.owner_class;observed.outer=p.map_witness.outer;
    observed.asset=probe().target.asset;observed.mesh_node=probe().target.mesh_node;
    text(observed.asset.name,"inncrate1");text(observed.mesh_node.name,"inncrate1");
    const IndexedDraw args{true,true,0,1,0,3};
    DrawObservation state;state.available=true;state.view=state.projection=state.world=identity;
    state.viewport={0,0,64,64,0,.5f};
    auto present=[&] {const auto t=driver->BeginPresent(thread);return driver->EndPresent(t,thread,true);};
    auto sample=[&] {const auto t=driver->BeginProbe(thread);if(!t)return false;driver->EndProbe(t,thread,p);return true;};
    auto submit=[&](const SubmissionSample& value,const ProbeStaticMesh* evidence,bool expected) {
        const auto scope=driver->BeginScope(thread,1,0);if (!scope)return false;
        auto cheap=value;cheap.asset_available=false;cheap.asset_proxy=cheap.asset_resource=0;std::memset(cheap.asset_name,0,sizeof(cheap.asset_name));
        const auto available=driver->GenericProbe(scope,thread,cheap);
        if (bool(available)!=bool(evidence)) return false;
        if (!driver->SetScopeSample(scope,thread,value,evidence))return false;
        const auto draw=driver->BeginDraw(thread,scope,args);
        if (!draw.ticket || draw.needs_state!=expected)return false;
        driver->EndDraw(draw.ticket,thread,true,state,state);driver->EndScope(scope,thread,value);return true;
    };
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(before,&observed,true));
    CHECK(present()==WindowResult::Published);auto current=driver->current();
    CHECK(current && current->meshes.size()==1 && current->meshes[0].selection.instance_id==actor_id &&
          current->meshes[0].selection.generation==1 && current->meshes[0].selection.binding==BindingEvidence::Candidate);
    before.matrix[12]=bits(2.5f);observed.entry.object_matrix_bits[12]=bits(2.5f);state.world[12]=2.5f;
    CHECK(sample());CHECK(submit(before,&observed,true));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes[0].selection.transform[12]==2.5f && driver->current()->meshes[0].selection.generation==1);
    state.world=identity;CHECK(sample());CHECK(submit(bsp_scope(),nullptr,true));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes.empty());state.world[12]=2.5f;
    CHECK(sample());CHECK(submit(before,&observed,true));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes[0].selection.generation==2); // an absent interval may hide identity reuse
    auto bad=observed;text(bad.mesh_node.sections[0].material_name,"Other");
    CHECK(sample());CHECK(submit(before,&bad,false));state.world=identity;CHECK(submit(bsp_scope(),nullptr,true));
    CHECK(present()==WindowResult::Published && driver->current()->meshes.empty());
    const auto reset=driver->BeginReset(thread);CHECK(reset);driver->EndReset(reset,thread,true);
    CHECK(present()==WindowResult::Primed);state.world[12]=2.5f;
    CHECK(sample());CHECK(submit(before,&observed,true));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes[0].selection.generation==3);
    CHECK(driver->stats().static_mesh_samples==5 && driver->stats().static_mesh_scopes==4 &&
          driver->stats().static_mesh_rejected==1);
    CHECK(sample());const auto scope=driver->BeginScope(thread,1,0);CHECK(scope);
    CHECK(driver->SetScopeSample(scope,thread,before,&observed));const auto draw=driver->BeginDraw(thread,scope,args);
    CHECK(draw.ticket && draw.needs_state);auto changed=state;changed.world[12]+=1;
    driver->EndDraw(draw.ticket,thread,true,state,changed);driver->EndScope(scope,thread,before);
    CHECK(present()==WindowResult::Rejected && !driver->current());
    CHECK(sample());CHECK(submit(before,&observed,true));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes[0].selection.generation==4); // rejected interval can hide reuse
    CHECK(sample());const auto overlapping=driver->BeginScope(thread,1,0);CHECK(overlapping);
    const auto stale=driver->GenericProbe(overlapping,thread,before);CHECK(stale);
    CHECK(present()==WindowResult::Rejected && !driver->current());
    CHECK(driver->SetScopeSample(overlapping,thread,before,&observed));driver->EndScope(overlapping,thread,before);
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(before,&observed,true));
    CHECK(present()==WindowResult::Published && driver->current()->meshes[0].selection.generation==5);
    return true;
}
bool delivery() {
    auto c=catalog();auto p=probe();auto bed=bed_scope();auto block=bsp_scope();
    SceneResidency state;CHECK(state.BeginMap(0,c.map,c.map_sha256));auto epoch=state.epochs();
    CHECK(state.AddMaterial(epoch.map,c.materials.at("mat:stone")));CHECK(state.AddResource(epoch.map,*c.bed));
    CHECK(state.AddResource(epoch.map,*c.bsp[0]));
    FrameProvenance provenance;provenance.origin=FrameOrigin::Synthetic;provenance.session_id="binding-fixture";
    Conventions conventions;conventions.length_unit=LengthUnit::T3Unit;conventions.axes=Axes::T3Xyz;conventions.up_axis=UpAxis::Z;
    MeshSelection foreign;CHECK(ResolveBed(c,p,bed,0,1,foreign));
    FrameInput invalid;invalid.map_epoch=epoch.map;invalid.device_epoch=epoch.device;invalid.frame_id=2;
    invalid.provenance=provenance;invalid.conventions=conventions;invalid.synchronized=true;
    invalid.camera.view=identity;invalid.camera.projection=identity;invalid.camera.width=640;invalid.camera.height=480;
    invalid.meshes.push_back(foreign);CHECK(!state.Publish(invalid)); // a same-ID foreign resource is not the resident object
    c.bed=state.FindSceneResource(epoch.map,bed_id);c.bsp[0]=state.FindSceneResource(epoch.map,bsp_id);
    CHECK(c.bed && c.bsp[0] && !state.FindSceneResource(epoch.map+1,bed_id));
    auto other_catalog=c;other_catalog.materials.at("mat:stone").name="Other";
    auto other_probe=p;text(other_probe.target.mesh_node.sections[0].material_name,"Other");
    CHECK(ResolveBed(other_catalog,other_probe,bed,0,1,foreign));invalid.meshes[0]=foreign;
    CHECK(!state.Publish(invalid)); // the catalog material name differs from residency
    text(other_probe.bsp.candidates[0].sections[0].material_name,"Other");
    BspSelection foreign_bsp;CHECK(ResolveBsp(other_catalog,other_probe,block,0,foreign_bsp));
    invalid.meshes.clear();invalid.bsp.push_back(foreign_bsp);CHECK(!state.Publish(invalid));
    SceneWindow window(state,epoch,provenance,conventions,0,1);const ThreadToken thread{17,123};
    auto present=window.BeginPresent(thread,1,true);CHECK(present);CHECK(window.EndPresent(present,thread,true)==WindowResult::Primed);
    auto probe_ticket=window.BeginProbe(thread);CHECK(probe_ticket);t3render::live::ProbeCamera cam;
    CHECK(ReadProbeCamera(p,17,0,cam));CHECK(window.EndProbe(probe_ticket,thread,true,0,true,cam));
    MeshSelection m;CHECK(ResolveBed(c,p,bed,0,1,m));auto scope=window.BeginMesh(thread,0,1,m);CHECK(scope);
    auto draw=window.BeginDraw(thread);CHECK(draw);const Viewport vp{0,0,640,480,0,0.5f};
    CHECK(window.EndDraw(draw,thread,true,ScopeDrawMatches(*c.bed,0,{true,true,0,1,0,3}),ScopeCameraMatches(cam,identity,identity),ScopeWorldMatches(bed,identity),true,vp));CHECK(window.EndScope(scope,thread,ScopeUnchanged(bed,bed)));
    BspSelection b;CHECK(ResolveBsp(c,p,block,0,b));scope=window.BeginBsp(thread,0,1,b);CHECK(scope);
    draw=window.BeginDraw(thread);CHECK(draw);CHECK(window.EndDraw(draw,thread,true,ScopeDrawMatches(*c.bsp[0],0,{true,true,0,1,0,3}),ScopeCameraMatches(cam,identity,identity),ScopeWorldMatches(block,identity),true,vp));CHECK(window.EndScope(scope,thread,ScopeUnchanged(block,block)));
    present=window.BeginPresent(thread,2,true);CHECK(present);CHECK(window.EndPresent(present,thread,true)==WindowResult::Published);
    auto snap=state.current();CHECK(snap && snap->frame_id==2 && snap->meshes.size()==1 && snap->bsp.size()==1);
    CHECK(snap->meshes[0].selection.binding==BindingEvidence::Candidate && snap->bsp[0].selection.binding==BindingEvidence::Candidate);
    probe_ticket=window.BeginProbe(thread);CHECK(probe_ticket);CHECK(window.EndProbe(probe_ticket,thread,true,0,true,cam));
    CHECK(ResolveBed(c,p,bed,0,1,m));scope=window.BeginMesh(thread,0,1,m);CHECK(scope);draw=window.BeginDraw(thread);CHECK(draw);
    CHECK(window.EndDraw(draw,thread,true,ScopeDrawMatches(*c.bed,0,{true,true,0,1,0,3}),ScopeCameraMatches(cam,identity,identity),ScopeWorldMatches(bed,identity),true,vp));
    auto changed=bed;changed.owner++;CHECK(!window.EndScope(scope,thread,ScopeUnchanged(bed,changed)));
    present=window.BeginPresent(thread,3,true);CHECK(present);CHECK(window.EndPresent(present,thread,true)==WindowResult::Rejected);
    CHECK(!state.current());
    return true;
}

bool driver_delivery() {
    auto c=catalog();auto memory=std::make_shared<SceneResidency>();CHECK(memory->BeginMap(0,c.map,c.map_sha256));
    const auto epoch=memory->epochs();CHECK(memory->AddMaterial(epoch.map,c.materials.at("mat:stone")));
    CHECK(memory->AddResource(epoch.map,*c.bed));CHECK(memory->AddResource(epoch.map,*c.bsp[0]));
    c.bed=memory->FindSceneResource(epoch.map,bed_id);c.bsp[0]=memory->FindSceneResource(epoch.map,bsp_id);
    LoadedCatalog loaded;loaded.residency=memory;loaded.binding=c;
    loaded.conventions.length_unit=LengthUnit::T3Unit;loaded.conventions.axes=Axes::T3Xyz;loaded.conventions.up_axis=UpAxis::Z;
    FrameProvenance source;source.origin=FrameOrigin::Synthetic;source.session_id="driver-fixture";
    auto driver=SceneDriver::Create(std::move(loaded),source,0,1);CHECK(driver);
    const ThreadToken thread{17,123};auto p=probe();const auto bed=bed_scope();const auto bsp=bsp_scope();
    const IndexedDraw args{true,true,0,1,0,3};
    DrawObservation state;state.available=true;state.view=state.projection=state.world=identity;state.viewport={0,0,640,480,0,0.5f};
    auto present=[&](bool ok=true) {auto t=driver->BeginPresent(thread);return driver->EndPresent(t,thread,ok);};
    auto sample=[&]() {auto t=driver->BeginProbe(thread);if (!t)return false;driver->EndProbe(t,thread,p);return true;};
    auto submit=[&](const SubmissionSample& before, const DrawObservation& endstate) {
        auto scope=driver->BeginScope(thread,1,0);if (!scope || !driver->SetScopeSample(scope,thread,before))return false;
        auto draw=driver->BeginDraw(thread,scope,args);if (!draw.ticket || !draw.needs_state)return false;
        driver->EndDraw(draw.ticket,thread,true,state,endstate);driver->EndScope(scope,thread,before);return true;
    };
    std::uint64_t live_frame=UINT64_MAX;
    auto live_ticket=driver->BeginPresent(thread,&live_frame);
    CHECK(live_ticket && live_frame==0 && driver->EndPresent(live_ticket,thread,true)==WindowResult::Primed);
    VisualLight light;light.id="light:candle";light.generation=1;light.binding=BindingEvidence::Candidate;
    light.position={Vec3{1,2,3},BindingEvidence::Candidate};
    CHECK(!driver->BeginLightSample(thread));
    CHECK(sample());auto light_ticket=driver->BeginLightSample(thread);CHECK(light_ticket);
    CHECK(driver->EndLightSample(light_ticket,thread,true,LightCoverage::Partial,{light}));
    CHECK(submit(bed,state));CHECK(submit(bsp,state));CHECK(submit(bsp,state));
    live_ticket=driver->BeginPresent(thread,&live_frame);
    CHECK(live_ticket && live_frame==1 && driver->EndPresent(live_ticket,thread,true)==WindowResult::Published);
    auto first=driver->current();
    CHECK(first && first->frame_id==1 && first->meshes.size()==1 && first->bsp.size()==1);
    CHECK(first->light_coverage==LightCoverage::Partial && first->lights.size()==1 && first->lights[0]==light);
    auto diagnostics=driver->stats();
    CHECK(diagnostics.published==1 && diagnostics.selected_probes==1 && diagnostics.bed_scopes==1 &&
          diagnostics.bsp_scopes==2 && diagnostics.bsp_cache_hits==1 && diagnostics.last_rejection==0);
    CHECK(first->meshes[0].selection.generation==1 && first->provenance.origin==FrameOrigin::Synthetic);
    p=without_bed();CHECK(sample());CHECK(submit(bsp,state));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes.empty() && driver->current()->bsp.size()==1 &&
          driver->current()->lights.empty() && driver->current()->light_coverage==LightCoverage::Unknown);
    p=probe();
    CHECK(sample());CHECK(submit(bed,state));CHECK(present()==WindowResult::Published);
    CHECK(driver->current()->meshes[0].selection.generation==2); // gap could hide identity reuse
    CHECK(sample());auto moved=state;moved.world[12]=1;CHECK(submit(bed,moved));
    CHECK(present()==WindowResult::Rejected && !driver->current());
    CHECK(driver->stats().last_rejection & WindowTainted);
    CHECK(sample());CHECK(submit(bed,state));CHECK(present()==WindowResult::Published);
    auto reset=driver->BeginReset(thread);CHECK(reset && !driver->current());driver->EndReset(reset,thread,false);
    CHECK(present()==WindowResult::Rejected && !driver->current());
    reset=driver->BeginReset(thread);CHECK(reset);driver->EndReset(reset,thread,true);
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bed,state));CHECK(present()==WindowResult::Published);
    CHECK(sample());auto outer=driver->BeginScope(thread,1,0);CHECK(outer && driver->SetScopeSample(outer,thread,bed));
    auto inner=driver->BeginScope(thread,1,0);CHECK(inner);CHECK(!driver->SetScopeSample(inner,thread,bsp));
    CHECK(present()==WindowResult::Rejected && !driver->current());
    driver->EndScope(inner,thread,bsp);driver->EndScope(outer,thread,bed);
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bsp,state));CHECK(present()==WindowResult::Published);
    // A Present during process-memory sampling must not publish a stale frame.
    CHECK(sample());auto sampling=driver->BeginScope(thread,1,0);CHECK(sampling);
    CHECK(present()==WindowResult::Rejected && !driver->current());
    (void)driver->SetScopeSample(sampling,thread,bed);driver->EndScope(sampling,thread,bed);
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bed,state));CHECK(present()==WindowResult::Published);
    // A Reset spanning a draw retires the old epoch before the draw returns.
    CHECK(sample());auto scope=driver->BeginScope(thread,1,0);CHECK(scope && driver->SetScopeSample(scope,thread,bed));
    auto draw=driver->BeginDraw(thread,scope,args);CHECK(draw.ticket && draw.needs_state);
    reset=driver->BeginReset(thread);CHECK(reset && !driver->current());driver->EndReset(reset,thread,true);
    driver->EndDraw(draw.ticket,thread,true,state,state);driver->EndScope(scope,thread,bed);
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bed,state));CHECK(present()==WindowResult::Published);
    driver->MarkLost();CHECK(!driver->current());
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bed,state));CHECK(present()==WindowResult::Published);
    CHECK(sample());CHECK(submit(bsp,state));
    auto replaced=bsp;replaced.bsp_source++;
    scope=driver->BeginScope(thread,1,0);CHECK(scope && !driver->SetScopeSample(scope,thread,replaced));
    driver->EndScope(scope,thread,replaced);CHECK(present()==WindowResult::Primed && !driver->current());
    CHECK(sample());CHECK(submit(bsp,state));CHECK(present()==WindowResult::Published);
    CHECK(sample());light_ticket=driver->BeginLightSample(thread);CHECK(light_ticket);
    CHECK(present()==WindowResult::Rejected && !driver->current());
    CHECK(!driver->EndLightSample(light_ticket,thread,true,LightCoverage::Partial,{light}));
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bsp,state));CHECK(present()==WindowResult::Published);
    CHECK(sample());light_ticket=driver->BeginLightSample(thread);CHECK(light_ticket);
    reset=driver->BeginReset(thread);CHECK(reset && !driver->current());driver->EndReset(reset,thread,true);
    CHECK(!driver->EndLightSample(light_ticket,thread,true,LightCoverage::Partial,{light}));
    CHECK(present()==WindowResult::Primed);CHECK(sample());CHECK(submit(bsp,state));CHECK(present()==WindowResult::Published);
    return true;
}
#ifdef T3RENDER_TEST_LIVE_DX12_DRIVER
struct PipelineFixture { std::unique_ptr<SceneDriver> driver; };
std::shared_ptr<const Snapshot> pipeline_current(void* context) noexcept {
    return static_cast<PipelineFixture*>(context)->driver->current();
}
bool pipeline_submit(void* context,const std::shared_ptr<const Snapshot>& expected,
                     SubmitOperation submit,void* argument) noexcept {
    return static_cast<PipelineFixture*>(context)->driver->SubmitIfCurrent(expected,submit,argument);
}
bool pipeline_wait(SceneViewer& viewer,std::uint64_t frames,std::uint64_t cleared=0) {
    const auto deadline=GetTickCount64()+20000;
    while (GetTickCount64()<deadline) {
        const auto stats=viewer.stats();
        if (stats.failed || stats.stopped) return false;
        if (stats.initialized && stats.frames>=frames && stats.cleared>=cleared) return true;
        Sleep(10);
    }
    return false;
}
bool driver_gpu_pipeline() {
    auto c=catalog();auto memory=std::make_shared<SceneResidency>();CHECK(memory->BeginMap(0,c.map,c.map_sha256));
    const auto epoch=memory->epochs();CHECK(memory->AddMaterial(epoch.map,c.materials.at("mat:stone")));
    CHECK(memory->AddResource(epoch.map,*c.bed));CHECK(memory->AddResource(epoch.map,*c.bsp[0]));
    c.bed=memory->FindSceneResource(epoch.map,bed_id);c.bsp[0]=memory->FindSceneResource(epoch.map,bsp_id);
    LoadedCatalog loaded;loaded.residency=memory;loaded.binding=c;
    loaded.conventions.length_unit=LengthUnit::T3Unit;loaded.conventions.axes=Axes::T3Xyz;loaded.conventions.up_axis=UpAxis::Z;
    loaded.conventions.meters_per_unit=.01905f;
    FrameProvenance source;source.origin=FrameOrigin::Synthetic;source.session_id="driver-gpu-fixture";
    auto state=std::make_shared<PipelineFixture>();state->driver=SceneDriver::Create(std::move(loaded),source,0,1);
    CHECK(state->driver);
    ViewerProvider provider;provider.context=state;provider.current=pipeline_current;provider.submit_if_current=pipeline_submit;
    auto viewer=SceneViewer::Start(std::move(provider),false);CHECK(viewer && pipeline_wait(*viewer,0));
    auto& driver=*state->driver;const ThreadToken thread{17,123};auto p=probe();
    const auto bed=bed_scope(),bsp=bsp_scope();const IndexedDraw args{true,true,0,1,0,3};
    DrawObservation draw_state;draw_state.available=true;draw_state.view=draw_state.projection=draw_state.world=identity;
    draw_state.viewport={0,0,64,64,0,0.5f};
    auto present=[&] {const auto t=driver.BeginPresent(thread);return driver.EndPresent(t,thread,true);};
    auto sample=[&] {const auto t=driver.BeginProbe(thread);if (!t)return false;driver.EndProbe(t,thread,p);return true;};
    auto submit=[&](const SubmissionSample& value) {
        const auto scope=driver.BeginScope(thread,1,0);if (!scope || !driver.SetScopeSample(scope,thread,value))return false;
        const auto draw=driver.BeginDraw(thread,scope,args);if (!draw.ticket || !draw.needs_state)return false;
        driver.EndDraw(draw.ticket,thread,true,draw_state,draw_state);driver.EndScope(scope,thread,value);return true;
    };
    CHECK(present()==WindowResult::Primed);CHECK(sample());
    VisualLight light;light.id="light:fixture";light.generation=1;light.binding=BindingEvidence::Verified;
    light.kind={LightKind::Point,BindingEvidence::Verified};light.enabled={true,BindingEvidence::Verified};
    light.position={Vec3{1,2,3},BindingEvidence::Verified};light.color_linear={Vec3{1,.5f,.25f},BindingEvidence::Verified};
    light.intensity={2,BindingEvidence::Verified};light.influence_radius={100,BindingEvidence::Verified};
    light.source_radius_m={.05f,BindingEvidence::Verified};
    const auto light_ticket=driver.BeginLightSample(thread);CHECK(light_ticket);
    CHECK(driver.EndLightSample(light_ticket,thread,true,LightCoverage::Complete,{light}));
    CHECK(submit(bed));CHECK(submit(bsp));
    CHECK(present()==WindowResult::Published);viewer->Notify();CHECK(pipeline_wait(*viewer,1));
    CHECK(viewer->stats().frame_id==1 && driver.current()->meshes.size()==1 && driver.current()->bsp.size()==1 &&
          driver.current()->light_coverage==LightCoverage::Complete && driver.current()->lights.size()==1 &&
          driver.current()->lights[0]==light);
    VerifiedLighting lighting;CHECK(BuildVerifiedLighting(*driver.current(),lighting)==LightingResult::Ready &&
                                    LightingMatchesSnapshot(*driver.current(),lighting) && lighting.lights.size()==1);
    const auto reset=driver.BeginReset(thread);CHECK(reset);viewer->Notify();CHECK(pipeline_wait(*viewer,1,1));
    driver.EndReset(reset,thread,true);CHECK(present()==WindowResult::Primed);
    p.current_camera.derived_matrix_bits[12]=bits(0.25f);draw_state.view[12]=0.25f;
    CHECK(sample());CHECK(submit(bed));CHECK(submit(bsp));CHECK(present()==WindowResult::Published);
    viewer->Notify();CHECK(pipeline_wait(*viewer,2,1));
    CHECK(viewer->stats().frame_id==3 && driver.current()->camera.view[12]==0.25f &&
          driver.current()->light_coverage==LightCoverage::Unknown && driver.current()->lights.empty());
    viewer->Stop();
    const auto deadline=GetTickCount64()+20000;
    while (!viewer->stats().stopped && GetTickCount64()<deadline) Sleep(10);
    CHECK(viewer->stats().stopped && !viewer->stats().failed);
    std::cout<<"scene_live_driver_dx12_fixture=ok source=synthetic light_same_window=ok frame_and_reset=ok\n";
    return true;
}
#endif
} // namespace
int main() {
    if(!matching() || !static_mesh_matching() || !static_mesh_driver_lifecycle() || !delivery() || !driver_delivery())return 1;
#ifdef T3RENDER_TEST_LIVE_DX12_DRIVER
    if (!driver_gpu_pipeline()) return 1;
#endif
    std::cout<<"scene_live_binding_fixture=ok\n";
}
