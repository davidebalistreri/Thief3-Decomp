#include "../ir/scene_live.hpp"
#include "../src/scene_live_flat.hpp"

#include <atomic>
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
    malformed = *scene.current(); malformed.conventions.clip_depth=ClipDepth::MinusOneToOne;
    CHECK(!BuildFlatFrame(malformed, flat));
    third.camera.view[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!scene.Publish(third));
    CHECK(scene.current()->frame_id == 3);
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
} // namespace

int main() {
    if (!updates_and_flat_adapter() || !atomic_validation_and_generations() ||
        !reset_and_map_change() || !concurrent_readers()) return 1;
    std::cout << "scene_live_fixture=ok\n";
}
