#include "../src/scene_live_light_diagnostics.hpp"
#include <iostream>

int main() {
    using namespace t3render::live;
    VisualLight light;light.id="light:\"\\\n\xc3\xa8";light.generation=2;light.binding=BindingEvidence::Candidate;
    light.position={Vec3{1,2,3},BindingEvidence::Candidate};
    std::vector<VisualLight> lights(33,light);
    lights[1].position={};
    std::cout<<'{';
    // The writer appends fields to an existing record.
    std::cout<<"\"event\":\"frame\"";WriteLightDetails(std::cout,lights);std::cout<<"}\n";
    t3render::scene_bridge::VisibleLightCandidates sample;
    sample.stable=true;sample.listed=sample.count=1;std::strcpy(sample.items[0].actor_name,"Light");
    sample.items[0].actor_number=16;auto& words=sample.items[0].words;
    words.position_34[0]=0x3f800000;words.position_34[1]=0x40000000;words.position_34[2]=0x40400000;
    words.words_58_to_90[3]=5;words.words_58_to_90[4]=0x3f000000;
    words.words_58_to_90[10]=0xdeadbeef;words.words_58_to_90[11]=0xcafebabe;
    std::vector<VisualLight> converted;
    if (!BuildVisibleLightCandidates(sample,"bedroom",std::string(64,'a'),2,converted)) return 1;
    const auto batch=BuildRawLightBatch(sample,"bedroom",std::string(64,'a'),2,converted);if (!batch) return 1;
    Snapshot snapshot;snapshot.map="bedroom";snapshot.map_sha256=std::string(64,'a');
    snapshot.map_epoch=1;snapshot.device_epoch=2;snapshot.frame_id=3;snapshot.continuity=4;
    snapshot.provenance.origin=FrameOrigin::Synthetic;snapshot.provenance.session_id="test";
    snapshot.light_coverage=LightCoverage::Partial;snapshot.lights=converted;
    RawLightFrame raw;raw.map_epoch=1;raw.device_epoch=2;raw.frame_id=3;raw.continuity=4;
    raw.provenance=snapshot.provenance;raw.camera=snapshot.camera;raw.batch=batch;
    std::cout<<"{\"event\":\"raw\"";WriteRawLightDetails(std::cout,snapshot,&raw);std::cout<<"}\n";
    ++raw.frame_id;
    std::cout<<"{\"event\":\"stale\"";WriteRawLightDetails(std::cout,snapshot,&raw);std::cout<<"}\n";
    --raw.frame_id;raw.camera.view[12]=1;
    std::cout<<"{\"event\":\"camera_mismatch\"";WriteRawLightDetails(std::cout,snapshot,&raw);std::cout<<"}\n";
    snapshot.provenance.origin=FrameOrigin::CapturedReplay;snapshot.provenance.session_id="test\"session";
    snapshot.provenance.capture_sha256=std::string(64,'a');
    std::cout<<"{\"event\":\"identity\"";WriteFrameIdentity(std::cout,snapshot);std::cout<<"}\n";
}
