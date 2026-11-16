#include "../ir/scene_live_window.hpp"

#include <iostream>

using namespace t3render::live;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return false; } } while (0)
namespace {
const ThreadToken thread{17, 12345}, other{17, 12346};
const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
const std::string hash(64, 'a');
Viewport viewport{0,0,1024,768,0,0.5f};
ProbeCamera camera{identity,identity};

bool setup(SceneResidency& scene) {
    CHECK(scene.BeginMap(0,"bedroom",hash));
    const auto epoch=scene.epochs().map;
    CHECK(scene.AddMaterial(epoch,{"mat:wall","wall"}));
    Resource mesh; mesh.id="mesh:bed"; mesh.name="bed"; mesh.source_sha256=hash;
    mesh.geometry.positions={{0,0,0},{1,0,0},{0,1,0}};
    mesh.geometry.indices={0,1,2,0,1,2};
    mesh.geometry.sections={{"one",0,3,"mat:wall"},{"two",3,3,"mat:wall"}};
    CHECK(scene.AddResource(epoch,mesh));
    mesh.id="bsp:wall";mesh.name="wall";mesh.kind=ResourceKind::BspBlock;
    CHECK(scene.AddResource(epoch,mesh));
    return true;
}
FrameProvenance provenance() {
    FrameProvenance p; p.origin=FrameOrigin::Synthetic; p.session_id="window-fixture"; return p;
}
Conventions conventions() {
    Conventions c; c.length_unit=LengthUnit::T3Unit;c.axes=Axes::T3Xyz;c.up_axis=UpAxis::Z;return c;
}
MeshSelection mesh(unsigned section=0) {
    MeshSelection m;m.instance_id="actor:bed";m.resource_id="mesh:bed";m.generation=1;
    m.binding=BindingEvidence::Verified;m.transform=identity;m.sections={section};return m;
}
BspSelection bsp(unsigned section=0) {
    BspSelection b;b.resource_id="bsp:wall";b.binding=BindingEvidence::Verified;b.sections={section};return b;
}
bool prime(SceneWindow& window, std::uint64_t frame_id) {
    auto p=window.BeginPresent(thread,frame_id,true);CHECK(p);CHECK(window.EndPresent(p,thread,true)==WindowResult::Primed);
    return true;
}
bool probe(SceneWindow& window, ProbeCamera value=camera) {
    auto p=window.BeginProbe(thread);CHECK(p);CHECK(window.EndProbe(p,thread,true,0,true,value));return true;
}
bool submit_mesh(SceneWindow& window, MeshSelection value=mesh()) {
    auto s=window.BeginMesh(thread,0,1,value);CHECK(s);auto d=window.BeginDraw(thread);CHECK(d);
    CHECK(window.EndDraw(d,thread,true,true,viewport));CHECK(window.EndScope(s,thread,true));return true;
}
bool submit_bsp(SceneWindow& window, BspSelection value=bsp()) {
    auto s=window.BeginBsp(thread,0,1,value);CHECK(s);auto d=window.BeginDraw(thread);CHECK(d);
    CHECK(window.EndDraw(d,thread,true,true,viewport));CHECK(window.EndScope(s,thread,true));return true;
}
WindowResult end(SceneWindow& window, std::uint64_t frame_id, bool succeeded=true) {
    auto p=window.BeginPresent(thread,frame_id,true);
    return window.EndPresent(p,thread,succeeded);
}

bool valid_sequence() {
    SceneResidency scene;CHECK(setup(scene));
    SceneWindow window(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(!window.BeginProbe(thread)); // no prior Present boundary
    CHECK(prime(window,100));
    auto unscoped=window.BeginDraw(thread);CHECK(unscoped);
    CHECK(window.EndDraw(unscoped,thread,true,false,Viewport{})); // tracked, but no viewport needed
    CHECK(probe(window));
    CHECK(!window.BeginMesh(thread,0,2,mesh())); // another stage is not selected
    CHECK(submit_mesh(window,mesh(0)));CHECK(submit_mesh(window,mesh(1)));CHECK(submit_mesh(window,mesh(1)));
    CHECK(submit_bsp(window,bsp(1)));
    CHECK(end(window,101)==WindowResult::Published);
    auto first=scene.current();CHECK(first && first->frame_id==101 && first->camera.max_depth==0.5f);
    CHECK(first->provenance.origin==FrameOrigin::Synthetic && first->meshes.size()==1 && first->bsp.size()==1);
    CHECK(first->meshes[0].selection.sections==std::vector<std::uint32_t>({0,1}));
    auto moved=camera;moved.view[12]=4;
    CHECK(probe(window,moved));CHECK(submit_bsp(window));CHECK(end(window,102)==WindowResult::Published);
    CHECK(scene.current()->meshes.empty() && scene.current()->camera.view[12]==4);
    CHECK(first->meshes.size()==1 && first->frame_id==101);
    CHECK(probe(window,moved));auto shifted=mesh();shifted.transform[12]=7;CHECK(submit_mesh(window,shifted));
    CHECK(end(window,103)==WindowResult::Published);
    CHECK(scene.current()->meshes[0].selection.transform[12]==7 && scene.current()->bsp.empty());
    return true;
}

bool invalid_intervals_recover() {
    SceneResidency scene;CHECK(setup(scene));SceneWindow window(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(window,0));CHECK(probe(window));CHECK(submit_mesh(window));CHECK(end(window,1)==WindowResult::Published);
    CHECK(probe(window));auto s=window.BeginMesh(thread,0,1,mesh());CHECK(s);auto d=window.BeginDraw(thread);CHECK(d);
    CHECK(!window.EndDraw(d,thread,false,true,viewport));CHECK(window.EndScope(s,thread,true));
    CHECK(end(window,2)==WindowResult::Rejected);CHECK(!scene.current());
    // The clean ending Present still primes a new interval.
    CHECK(probe(window));CHECK(submit_mesh(window));CHECK(end(window,3)==WindowResult::Published);
    CHECK(probe(window));s=window.BeginMesh(thread,0,1,mesh());CHECK(s);d=window.BeginDraw(thread);CHECK(d);
    CHECK(!window.EndDraw(d,thread,true,false,viewport));CHECK(window.EndScope(s,thread,true));
    CHECK(end(window,4)==WindowResult::Rejected);CHECK(!scene.current());
    CHECK(probe(window));CHECK(submit_mesh(window));CHECK(end(window,5)==WindowResult::Published);
    CHECK(probe(window));auto changed=camera;changed.view[12]=1;
    auto p=window.BeginProbe(thread);CHECK(p);CHECK(!window.EndProbe(p,thread,true,0,true,changed));
    CHECK(end(window,6)==WindowResult::Rejected);CHECK(!scene.current());
    CHECK(probe(window));CHECK(submit_bsp(window));
    CHECK(end(window,7)==WindowResult::Published);
    CHECK(probe(window));p=window.BeginProbe(thread);CHECK(p);CHECK(window.EndProbe(p,thread,true,1,true,camera));
    CHECK(!window.BeginMesh(thread,0,1,mesh())); // latest probe belongs to another pass
    CHECK(end(window,8)==WindowResult::Rejected);
    CHECK(probe(window));CHECK(submit_mesh(window));
    CHECK(end(window,10)==WindowResult::Rejected); // skipped Present ID
    CHECK(!scene.current());CHECK(probe(window));CHECK(submit_mesh(window));
    CHECK(end(window,11)==WindowResult::Published);
    CHECK(end(window,12,false)==WindowResult::Rejected);CHECK(!scene.current());
    CHECK(!window.BeginProbe(thread));CHECK(prime(window,13));
    CHECK(probe(window));CHECK(submit_mesh(window));
    auto scope=window.BeginBsp(thread,0,1,bsp());CHECK(scope);auto draw=window.BeginDraw(thread);CHECK(draw);
    auto different=viewport;different.max_depth=1;
    CHECK(!window.EndDraw(draw,thread,true,true,different));CHECK(window.EndScope(scope,thread,true));
    CHECK(end(window,14)==WindowResult::Rejected);CHECK(!scene.current());
    CHECK(probe(window));CHECK(submit_mesh(window));auto conflicting=mesh(1);conflicting.transform[12]=5;
    scope=window.BeginMesh(thread,0,1,conflicting);CHECK(scope);draw=window.BeginDraw(thread);CHECK(draw);
    CHECK(window.EndDraw(draw,thread,true,true,viewport));CHECK(!window.EndScope(scope,thread,true));
    CHECK(end(window,15)==WindowResult::Rejected);CHECK(!scene.current());
    CHECK(probe(window));scope=window.BeginMesh(thread,0,1,mesh());CHECK(scope);
    CHECK(window.EndScope(scope,thread,true)); // a scope with no draw does not establish visibility
    CHECK(end(window,16)==WindowResult::Rejected);CHECK(!scene.current());
    return true;
}

bool overlaps_and_invalidations() {
    SceneResidency scene;CHECK(setup(scene));
    SceneWindow first(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(first,20));CHECK(probe(first));CHECK(submit_mesh(first));CHECK(end(first,21)==WindowResult::Published);
    CHECK(probe(first));auto scope=first.BeginMesh(thread,0,1,mesh());CHECK(scope);
    auto present=first.BeginPresent(thread,22,true);CHECK(present);
    CHECK(first.EndPresent(present,thread,true)==WindowResult::Rejected);CHECK(!scene.current() && first.needs_rebind());
    CHECK(!first.EndScope(scope,thread,true)); // stale callback cannot be assigned to another frame
    SceneWindow second(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(second,23));CHECK(probe(second));CHECK(submit_mesh(second));CHECK(end(second,24)==WindowResult::Published);
    present=second.BeginPresent(thread,25,true);CHECK(present);CHECK(!second.BeginProbe(thread));
    CHECK(second.EndPresent(present,thread,true)==WindowResult::Rejected);CHECK(!scene.current() && second.needs_rebind());
    SceneWindow third(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(third,26));CHECK(probe(third));CHECK(submit_mesh(third));third.MarkLost();
    CHECK(third.needs_rebind() && !scene.current());CHECK(!third.BeginPresent(thread,27,true));
    SceneWindow fourth(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(fourth,27));CHECK(probe(fourth));CHECK(submit_mesh(fourth));CHECK(end(fourth,28)==WindowResult::Published);
    auto older=scene.current();CHECK(probe(fourth));CHECK(submit_mesh(fourth));
    const auto reset=scene.BeginReset();CHECK(reset);CHECK(scene.EndReset(reset,true));
    CHECK(end(fourth,29)==WindowResult::Rejected);CHECK(!scene.current() && fourth.needs_rebind());
    SceneWindow fresh(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(fresh,29));CHECK(probe(fresh));CHECK(submit_mesh(fresh));CHECK(end(fresh,30)==WindowResult::Published);
    fourth.MarkLost(); // stale device epoch cannot erase the fresh snapshot
    CHECK(scene.current() && scene.current()->frame_id==30 && older->frame_id==28);
    const auto e=scene.epochs();CHECK(!scene.DiscardFrame(e.map,e.device,29));
    auto unscoped_draw=fresh.BeginDraw(thread);CHECK(unscoped_draw);
    present=fresh.BeginPresent(thread,31,true);CHECK(present);
    CHECK(fresh.EndPresent(present,thread,true)==WindowResult::Rejected);CHECK(!scene.current() && fresh.needs_rebind());
    CHECK(!fresh.EndDraw(unscoped_draw,thread,true,true,viewport));
    SceneWindow concurrent(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(concurrent,32));present=concurrent.BeginPresent(thread,33,true);CHECK(present);
    CHECK(!concurrent.BeginPresent(thread,33,true));
    CHECK(concurrent.EndPresent(present,thread,true)==WindowResult::Rejected);CHECK(concurrent.needs_rebind());
    SceneWindow unquiet(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(!unquiet.BeginPresent(thread,34,false));CHECK(unquiet.needs_rebind());
    SceneWindow different_thread(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(different_thread,35));CHECK(probe(different_thread));CHECK(submit_mesh(different_thread));
    CHECK(end(different_thread,36)==WindowResult::Published);CHECK(probe(different_thread));
    CHECK(!different_thread.BeginDraw(other));CHECK(different_thread.needs_rebind() && !scene.current());
    SceneWindow nested(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(nested,36));CHECK(probe(nested));scope=nested.BeginMesh(thread,0,1,mesh());CHECK(scope);
    CHECK(!nested.BeginBsp(thread,0,2,bsp()));CHECK(nested.needs_rebind());
    SceneWindow bitwise(scene,scene.epochs(),provenance(),conventions(),0,1);
    CHECK(prime(bitwise,37));CHECK(probe(bitwise));auto changed=camera;changed.view[1]=-0.0f;
    auto token=bitwise.BeginProbe(thread);CHECK(token);CHECK(!bitwise.EndProbe(token,thread,true,0,true,changed));
    CHECK(end(bitwise,38)==WindowResult::Rejected);
    return true;
}
} // namespace

int main() {
    if (!valid_sequence() || !invalid_intervals_recover() || !overlaps_and_invalidations()) return 1;
    std::cout << "scene_live_window_fixture=ok\n";
}
