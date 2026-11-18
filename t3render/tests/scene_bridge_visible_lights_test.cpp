#include "../src/scene_live_visible_lights.hpp"
#include "../ir/scene_live_window.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>

using namespace t3render::scene_bridge;
struct Memory {
    std::map<std::uint32_t,std::uint8_t> bytes;
    std::uint32_t trigger=0,change=0,value=0;unsigned reads=0,occurrence=1,triggered=0;
    template<class T> void put(std::uint32_t a,T v) {
        unsigned char data[sizeof(v)];std::memcpy(data,&v,sizeof(v));
        for (std::size_t i=0;i<sizeof(v);++i) bytes[a+i]=data[i];
    }
    void region(std::uint32_t a,std::size_t n) {for (std::size_t i=0;i<n;++i) bytes[a+i]=0;}
    void name(std::uint32_t a,const char* s) {
        region(a+0x1c,64);for (std::size_t i=0;s[i];++i) put<std::uint8_t>(a+0x1c+i,s[i]);
    }
    static bool read(void* p,std::uint32_t a,void* out,std::size_t n) noexcept {
        auto& m=*static_cast<Memory*>(p);++m.reads;
        for (std::size_t i=0;i<n;++i) if (!m.bytes.count(a+i)) return false;
        for (std::size_t i=0;i<n;++i) static_cast<unsigned char*>(out)[i]=m.bytes[a+i];
        if (a==m.trigger && ++m.triggered==m.occurrence) m.put<std::uint32_t>(m.change,m.value);
        return true;
    }
};
Memory Fixture() {
    Memory m;
    m.put<std::uint32_t>(0x1074,1);m.put<std::uint32_t>(0x1078,4);m.put<std::uint32_t>(0x107c,0x3000);
    m.put<std::uint32_t>(0x3000,0x4000);m.put<std::uint32_t>(0x4000,0x10e4aa80);m.put<std::uint32_t>(0x4054,0x5000);
    for (unsigned i=0;i<3;++i) {m.put<std::uint32_t>(0x4024+4*i,0x100+i);m.put<std::uint32_t>(0x4034+4*i,0x200+i);}
    for (unsigned i=0;i<15;++i) m.put<std::uint32_t>(0x4058+4*i,0x300+i);
    m.put<std::uint32_t>(0x5004,0);m.put<std::uint32_t>(0x5018,0x6000);
    m.put<std::uint32_t>(0x5020,16u<<16);m.put<std::uint32_t>(0x50b8,0x4000);
    m.put<std::uint32_t>(0x6004,1);m.put<std::uint32_t>(0x6018,0);m.put<std::uint32_t>(0x6020,1);
    m.put<std::uint32_t>(0x10f3e4a0,0x7000);m.put<std::uint32_t>(0x10f3e4a4,2);m.put<std::uint32_t>(0x10f3e4a8,2);
    m.put<std::uint32_t>(0x7000,0x5000);m.put<std::uint32_t>(0x7004,0x6000);
    m.put<std::uint32_t>(0x10f7af1c,0x8000);m.put<std::uint32_t>(0x10f7af20,2);m.put<std::uint32_t>(0x10f7af24,2);
    m.put<std::uint32_t>(0x8000,0x9000);m.put<std::uint32_t>(0x8004,0x9100);
    m.name(0x9000,"Light");m.name(0x9100,"LOADGAME:SouthQuarter1_int1");
    return m;
}
ProbeSnapshot CandleFixture(Memory& m) {
    m=Fixture();m.name(0x9000,"D_384");m.put<std::uint32_t>(0x5020,6u<<16);
    m.put<std::uint32_t>(0x1050,1);m.put<std::uint32_t>(0x1058,0xa000);
    m.put<std::uint32_t>(0xa000,0xb000);m.put<std::uint32_t>(0xb000,0x10e4e8c0);
    m.put<std::uint32_t>(0xb044,4);m.put<std::uint32_t>(0xb054,0x5000);m.put<std::uint32_t>(0x50b4,0xb000);
    for (unsigned i=0;i<16;++i) m.put<std::uint32_t>(0xb004+4*i,i%5==0 ? 0x3f800000 : 0);
    m.put<std::uint32_t>(0x408c,0x3f800000);
    ProbeSnapshot snapshot;snapshot.self=0x1000;snapshot.count={1,true};snapshot.array={0xa000,true};
    snapshot.entries_attempted=1;snapshot.entries[0].list_index=0;snapshot.entries[0].address={0xb000,true};
    snapshot.entries[0].vtable={0x10e4e8c0,true};snapshot.entries[0].type={4,true};
    return snapshot;
}
int main() {
    Memory m=Fixture();ProbeReader reader{&m,Memory::read};
    auto s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");
    assert(s.stable && !s.truncated && s.listed==1 && s.count==1 && s.skipped==0);
    assert(!std::strcmp(s.items[0].actor_name,"Light") && s.items[0].actor_number==16 &&
           s.items[0].words.position_34[2]==0x202);
    std::vector<t3render::live::VisualLight> lights;
    assert(t3render::live::BuildVisibleLightCandidates(s,"SouthQuarter1_int1",std::string(64,'a'),2,lights));
    assert(lights.size()==1 && lights[0].id=="light-actor:SouthQuarter1_int1:"+std::string(64,'a')+":Light:16" &&
           lights[0].generation==2 && lights[0].binding==t3render::live::BindingEvidence::Candidate &&
           lights[0].position.evidence==t3render::live::BindingEvidence::Candidate &&
           lights[0].enabled.evidence==t3render::live::BindingEvidence::Unknown && !lights[0].color_linear.value);
    auto collision=s;collision.count=2;collision.items[1]=collision.items[0];
    std::strcpy(collision.items[1].actor_name,"Light__15");collision.items[1].actor_number=0;
    assert(t3render::live::BuildVisibleLightCandidates(collision,"SouthQuarter1_int1",std::string(64,'a'),2,lights) &&
           lights.size()==2 && lights[0].id!=lights[1].id);
    s.items[0].words.position_34[0]=0x7fc00000;
    assert(t3render::live::BuildVisibleLightCandidates(s,"SouthQuarter1_int1",std::string(64,'a'),2,lights) &&
           !lights[0].position.value && lights[0].position.evidence==t3render::live::BindingEvidence::Unknown);
    m=Fixture();s=SampleVisibleLightCandidates(reader,0x1000,"different");assert(s.stable && !s.count && s.skipped==1);
    m=Fixture();m.put<std::uint32_t>(0x7000,0x7777);s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");
    assert(s.stable && !s.count && s.skipped==1);
    m=Fixture();m.put<std::uint32_t>(0x4054,0x1111);s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");
    assert(s.stable && !s.count && s.skipped==1);
    m=Fixture();m.put<std::uint32_t>(0x1074,129);m.put<std::uint32_t>(0x1078,516);
    s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");assert(!s.stable && s.truncated && !s.count);
    m=Fixture();m.put<std::uint32_t>(0x1078,3);assert(!SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1").stable);
    // Two distinct objects with the same map/name are not a unique actor binding.
    m=Fixture();m.put<std::uint32_t>(0x1074,2);m.put<std::uint32_t>(0x1078,8);m.put<std::uint32_t>(0x3004,0xc000);
    m.put<std::uint32_t>(0xc000,0x10e4aa80);m.put<std::uint32_t>(0xc054,0xd000);
    for (unsigned i=0;i<3;++i) {m.put<std::uint32_t>(0xc024+4*i,0x100+i);m.put<std::uint32_t>(0xc034+4*i,0x200+i);}
    for (unsigned i=0;i<15;++i) m.put<std::uint32_t>(0xc058+4*i,0x300+i);
    m.put<std::uint32_t>(0xd004,2);m.put<std::uint32_t>(0xd018,0x6000);m.put<std::uint32_t>(0xd020,16u<<16);
    m.put<std::uint32_t>(0xd0b8,0xc000);m.put<std::uint32_t>(0x7008,0xd000);
    m.put<std::uint32_t>(0x10f3e4a4,3);m.put<std::uint32_t>(0x10f3e4a8,3);
    s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");assert(!s.stable && !s.count);
    // A changed membership array or header is rejected for the whole sample.
    for (const auto changed:{0x3000u,0x1074u,0x10f3e4a0u}) {
        m=Fixture();m.trigger=0x4058;m.change=changed;m.value=0x12345678;
        s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");assert(!s.stable && !s.count);
    }
    // A changing light, actor identity or owner link cannot produce a value.
    for (const auto changed:{0x4034u,0x5020u,0x50b8u}) {
        m=Fixture();m.trigger=0x4058;m.change=changed;m.value=0x12345678;
        s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");assert(s.stable && !s.count && s.skipped==1);
    }
    // The candle is followed from the visual list even when it no longer
    // appears in the separate renderer light list. Neither absence means off.
    auto candle_snapshot=CandleFixture(m);
    m.put<std::uint32_t>(0x1074,0);
    auto candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.status==CandleLinkStatus::Candidate && candle.factor_8c_bits==0x3f800000 &&
           candle.transform_available && candle.transform_bits[0]==0x3f800000);
    m.put<std::uint32_t>(0x408c,0);
    candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.status==CandleLinkStatus::Candidate && candle.factor_8c_bits==0);
    m.put<std::uint32_t>(0x50b8,0);
    candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.status==CandleLinkStatus::NoLink && candle.factor_8c_bits==0 &&
           candle.transform_available && candle.transform_bits[0]==0x3f800000);
    assert(SampleBedroomCandleLink(reader,candle_snapshot,"other").status==CandleLinkStatus::Unknown);
    for (const auto changed:{0x408cu,0x5020u,0x50b8u,0xa000u,0x50b4u}) {
        candle_snapshot=CandleFixture(m);m.trigger=0x4058;m.change=changed;m.value=0x12345678;
        assert(SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1").status==CandleLinkStatus::Unknown);
    }
    candle_snapshot=CandleFixture(m);m.trigger=0x4058;m.change=0xb004;m.value=0x12345678;
    candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.status==CandleLinkStatus::Candidate && !candle.transform_available);
    candle_snapshot=CandleFixture(m);m.put<std::uint32_t>(0x50b8,0);
    m.trigger=0x50b8;m.change=0xb004;m.value=0x12345678;
    candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.status==CandleLinkStatus::NoLink && !candle.transform_available);
    candle_snapshot=CandleFixture(m);m.put<std::uint32_t>(0x50b8,0);
    m.trigger=0x50b8;m.occurrence=2;m.change=0x50b8;m.value=0x4000;
    assert(SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1").status==CandleLinkStatus::Unknown);
    candle_snapshot=CandleFixture(m);m.put<std::uint32_t>(0xa004,0xb000);m.put<std::uint32_t>(0x1050,2);
    candle_snapshot.count.value=2;candle_snapshot.entries_attempted=2;
    candle_snapshot.entries[1]=candle_snapshot.entries[0];candle_snapshot.entries[1].list_index=1;
    assert(SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1").status==CandleLinkStatus::Unknown);
    // Observe the candidate property through the actual actor-link path.
    candle_snapshot=CandleFixture(m);
    m.put<std::uint32_t>(0x5000,0x10e40000);m.put<std::uint32_t>(0x501c,0);
    m.put<std::uint32_t>(0x5028,0xc000);m.put<std::uint16_t>(0xc00e,1);
    m.put<std::uint64_t>(0x10f7b5d0,(std::uint64_t{4}<<32)|(std::uint64_t{0x85cf}<<16));
    m.put<std::uint32_t>(0x10f3eda4,0xd000);m.put<std::uint32_t>(0xd008,0xe000);
    m.put<std::uint32_t>(0xe000+0x5cf*4,0x11000);m.put<std::uint32_t>(0x11000,0x10e4e7e0);
    m.put<std::uint32_t>(0x10e4e7e0+0xb0,0x10aed570);m.put<std::uint32_t>(0x10e4e7e0+0xb4,0x10aecb20);
    m.put<std::uint32_t>(0x11034,1);m.put<std::uint32_t>(0x11038,4);m.put<std::uint32_t>(0x11060,4);
    candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.enabled.status==CandleEnabledStatus::On && !candle.enabled.inherited);
    m.put<std::uint32_t>(0x10f7b5d4,0);
    candle=SampleBedroomCandleLink(reader,candle_snapshot,"SouthQuarter1_int1");
    assert(candle.enabled.status==CandleEnabledStatus::Off && !candle.enabled.inherited);
    // Exercise the raw collection through conversion and the actual Present
    // window; synthetic values remain candidates and cannot enable lighting.
    using namespace t3render::live;
    m=Fixture();SceneResidency scene;assert(scene.BeginMap(0,"SouthQuarter1_int1",std::string(64,'a')));
    const auto epoch=scene.epochs();Resource bsp;bsp.id="bsp:test";bsp.name="test";
    bsp.source_sha256=std::string(64,'b');bsp.kind=ResourceKind::BspBlock;
    bsp.geometry.positions={{0,0,0},{1,0,0},{0,1,0}};bsp.geometry.indices={0,1,2};
    bsp.geometry.sections={{"section",0,3,std::nullopt}};assert(scene.AddResource(epoch.map,std::move(bsp)));
    FrameProvenance provenance;provenance.origin=FrameOrigin::Synthetic;provenance.session_id="light-fixture";
    SceneWindow window(scene,epoch,provenance,Conventions{},0,1);ThreadToken thread{7,9};
    auto ticket=window.BeginPresent(thread,0,true);assert(ticket && window.EndPresent(ticket,thread,true)==WindowResult::Primed);
    t3render::live::ProbeCamera camera;camera.view=camera.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    ticket=window.BeginProbe(thread);assert(ticket && window.EndProbe(ticket,thread,true,0,true,camera));
    ticket=window.BeginLightSample(thread);assert(ticket);
    s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");
    assert(BuildVisibleLightCandidates(s,"SouthQuarter1_int1",std::string(64,'a'),2,lights));
    assert(window.EndLightSample(ticket,thread,true,LightCoverage::Partial,lights));
    BspSelection block;block.resource_id="bsp:test";block.binding=BindingEvidence::Candidate;block.sections={0};
    const auto scope=window.BeginBsp(thread,0,1,block);assert(scope);
    ticket=window.BeginDraw(thread);assert(ticket && window.EndDraw(ticket,thread,true,true,true,true,true,{0,0,640,480,0,.5f}));
    assert(window.EndScope(scope,thread,true));ticket=window.BeginPresent(thread,1,true);
    assert(ticket && window.EndPresent(ticket,thread,true)==WindowResult::Published);
    const auto snapshot=scene.current();assert(snapshot && snapshot->frame_id==1 && snapshot->lights.size()==1 &&
        snapshot->light_coverage==LightCoverage::Partial && ClassifyVisualLight(snapshot->lights[0])==LightReadiness::Incomplete);
    ticket=window.BeginProbe(thread);assert(ticket && window.EndProbe(ticket,thread,true,0,true,camera));
    ticket=window.BeginLightSample(thread);assert(ticket);
    m.trigger=0x4058;m.change=0x3000;m.value=0x12345678;
    s=SampleVisibleLightCandidates(reader,0x1000,"SouthQuarter1_int1");assert(!s.stable);
    assert(window.CancelLightSample(ticket,thread));
    const auto scope2=window.BeginBsp(thread,0,1,block);assert(scope2);
    ticket=window.BeginDraw(thread);assert(ticket && window.EndDraw(ticket,thread,true,true,true,true,true,{0,0,640,480,0,.5f}));
    assert(window.EndScope(scope2,thread,true));ticket=window.BeginPresent(thread,2,true);
    assert(ticket && window.EndPresent(ticket,thread,true)==WindowResult::Published &&
           scene.current()->light_coverage==LightCoverage::Unknown && scene.current()->lights.empty());
}
