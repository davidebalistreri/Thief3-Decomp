#include "../src/scene_live_framing.hpp"
#include "../src/scene_live_catalog.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>

using namespace t3render::live;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return 1; } } while (0)

namespace {
bool ExpectedDigest(const std::uint8_t*,std::size_t,std::uint8_t* output,void* context) noexcept {
    std::memcpy(output,context,32);return true;
}
}

int main(int argc,char** argv) {
    const std::string map="SouthQuarter1_int1";
    const std::string hash="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
    const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    Snapshot s;s.map=map;s.map_sha256=hash;s.map_epoch=1;s.device_epoch=2;s.continuity=3;s.frame_id=4;
    s.provenance.origin=FrameOrigin::Synthetic;s.provenance.session_id="fixture";
    s.conventions.length_unit=LengthUnit::T3Unit;s.conventions.axes=Axes::T3Xyz;s.conventions.up_axis=UpAxis::Z;
    s.conventions.meters_per_unit=0.01905f;
    s.camera.view=identity;s.camera.projection=identity;s.camera.width=1024;s.camera.height=768;
    auto source=std::make_shared<Resource>();source->name="innbed1a1";source->source_sha256=std::string(64,'b');
    source->id="mesh:"+map+":"+hash+":"+source->source_sha256+":innbed1a1";
    auto high=std::make_shared<Resource>();high->id="bed_high";
    high->geometry.positions={{-.5f,-.5f,.25f},{.5f,.5f,.75f}};
    MeshSelection selection;selection.instance_id="actor:"+map+":"+hash+":StaticMeshActor__1520";
    selection.resource_id=source->id;selection.generation=1;selection.binding=BindingEvidence::Candidate;
    selection.sections={0};selection.transform=identity;s.meshes.push_back({selection,source});
    MeshOverride bed{map,hash,selection.instance_id,source,high};
    BedroomFramingBounds bounds;
    CHECK(BedroomBedFraming(s,bed,bounds));
    CHECK(std::fabs(bounds.left-.25)<1e-6 && std::fabs(bounds.right-.75)<1e-6 &&
          std::fabs(bounds.top-.25)<1e-6 && std::fabs(bounds.bottom-.75)<1e-6);
    s.meshes[0].selection.transform[12]=1;CHECK(BedroomBedFraming(s,bed,bounds) && bounds.right>1);
    s.meshes[0].selection.transform[12]=0;
    high->geometry.positions[0][2]=-1;CHECK(!BedroomBedFraming(s,bed,bounds));
    high->geometry.positions[0][2]=.25f;
    s.camera.projection[11]=1;s.camera.projection[15]=0;
    CHECK(BedroomBedFraming(s,bed,bounds));
    s.meshes[0].selection.transform[14]=-2;CHECK(!BedroomBedFraming(s,bed,bounds));
    s.meshes[0].selection.transform=identity;s.camera.projection=identity;
    auto foreign=bed;foreign.source=std::make_shared<Resource>(*source);CHECK(!BedroomBedFraming(s,foreign,bounds));
    foreign=bed;foreign.map_sha256=std::string(64,'a');CHECK(!BedroomBedFraming(s,foreign,bounds));
    s.meshes.push_back(s.meshes[0]);CHECK(!BedroomBedFraming(s,bed,bounds));s.meshes.pop_back();
    high->geometry.positions[0][0]=std::numeric_limits<float>::quiet_NaN();CHECK(!BedroomBedFraming(s,bed,bounds));
    high->geometry.positions.clear();CHECK(!BedroomBedFraming(s,bed,bounds));
    high->geometry.positions={{-.5f,-.5f,.25f},{.5f,.5f,.75f}};
    s.camera.projection[0]=std::numeric_limits<float>::quiet_NaN();CHECK(!BedroomBedFraming(s,bed,bounds));
    s.camera.projection=identity;
    s.camera.width=0;CHECK(!BedroomBedFraming(s,bed,bounds));s.camera.width=1024;

    s.camera.projection={.02f,0,0,0, 0,.02f,0,0, 0,0,.001f,0, 0,0,.5f,1};
    s.light_coverage=LightCoverage::Partial;
    VisualLight light;light.id="light-actor:"+map+":"+hash+":D_384:14";light.generation=5;
    light.binding=BindingEvidence::Candidate;light.position={Vec3{0,0,0},BindingEvidence::Candidate};
    s.lights.push_back(light);
    RawLightItem item;item.candidate=light;
    const float color[]{1,.5f,.25f},full=1;
    std::memcpy(item.field_68.data(),color,sizeof(color));std::memcpy(&item.field_8c,&full,sizeof(full));
    auto batch=std::make_shared<RawLightBatch>();batch->listed=1;batch->items.push_back(item);
    batch->candle_actor_number=14;batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::Candidate;
    batch->candle_link.factor_8c_bits=item.field_8c;batch->candle_link.transform_available=true;
    std::memcpy(batch->candle_link.transform_bits.data(),identity.data(),sizeof(float)*identity.size());
    RawLightFrame raw;raw.map_epoch=1;raw.device_epoch=2;raw.continuity=3;raw.frame_id=4;
    raw.provenance=s.provenance;raw.camera=s.camera;raw.batch=batch;
    BedroomFramingBounds on,off;
    CHECK(BedroomCandleFraming(s,raw,on));
    batch->candle_link.enabled.status=t3render::scene_bridge::CandleEnabledStatus::Off;
    CHECK(BedroomCandleFraming(s,raw,off));
    CHECK(on.left<=off.left && on.right>=off.right && on.top<=off.top && on.bottom>=off.bottom);
    auto other_light=light;other_light.id="light-actor:"+map+":"+hash+":D_384:6";
    s.lights.push_back(other_light);auto other_item=item;other_item.candidate=other_light;
    batch->items.push_back(other_item);batch->listed=2;
    batch->other_candle_link=batch->candle_link;
    batch->other_candle_link.enabled.status=t3render::scene_bridge::CandleEnabledStatus::On;
    const float other_x=20;
    std::memcpy(&batch->other_candle_link.transform_bits[12],&other_x,sizeof(other_x));
    BedroomFramingBounds other_on,other_off;
    CHECK(BedroomCandleFraming(s,raw,other_on,6) && other_on.left>on.left);
    batch->other_candle_link.enabled.status=t3render::scene_bridge::CandleEnabledStatus::Off;
    CHECK(BedroomCandleFraming(s,raw,other_off,6) && other_on.top<=other_off.top);
    batch->other_candle_link.status=t3render::scene_bridge::CandleLinkStatus::Unknown;
    batch->other_candle_link.transform_available=false;batch->other_candle_link.transform_bits={};
    batch->other_candle_link.factor_8c_bits=0;
    batch->other_candle_link.enabled.status=t3render::scene_bridge::CandleEnabledStatus::Unknown;
    CHECK(!BedroomCandleFraming(s,raw,other_off,6) && BedroomCandleFraming(s,raw,off,14));
    s.lights.pop_back();batch->items.pop_back();batch->listed=1;batch->other_candle_link={};
    ++raw.frame_id;CHECK(!BedroomCandleFraming(s,raw,bounds));--raw.frame_id;
    raw.camera.view[12]=1;CHECK(!BedroomCandleFraming(s,raw,bounds));raw.camera=s.camera;
    batch->candle_link.enabled.status=t3render::scene_bridge::CandleEnabledStatus::Unknown;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    std::memcpy(&batch->items[0].field_8c,&nan,sizeof(nan));
    batch->candle_link.factor_8c_bits=batch->items[0].field_8c;
    CHECK(!BedroomCandleFraming(s,raw,bounds));
    raw.batch.reset();CHECK(!BedroomCandleFraming(s,raw,bounds));
    if (argc==2) {
        // The caller independently computes SHA-256 over the catalog body.
        CHECK(std::strlen(argv[1])==64);
        std::array<std::uint8_t,32> digest{};
        auto hex=[](char c) {return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1;};
        for (unsigned i=0;i<32;++i) {
            const int a=hex(argv[1][2*i]),b=hex(argv[1][2*i+1]);CHECK(a>=0 && b>=0);
            digest[i]=static_cast<std::uint8_t>((a<<4)|b);
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)),{});
        LoadedCatalog catalog;CHECK(LoadCatalog(bytes.data(),bytes.size(),ExpectedDigest,digest.data(),catalog));
        CHECK(catalog.bed_override && catalog.binding.bed);
        Snapshot captured;captured.map=catalog.binding.map;captured.map_sha256=catalog.binding.map_sha256;
        captured.conventions=catalog.conventions;
        captured.camera.width=1024;captured.camera.height=768;
        captured.camera.view={.368466794f,-.158933148f,-.915954351f,0,-.92964083f,-.0629937723f,-.363042086f,0,
                              0,.985277593f,-.170961872f,0,206.650391f,332.020844f,3985.35571f,1};
        captured.camera.projection={1.2060982f,0,0,0,0,1.60813093f,0,0,0,0,1.00003123f,1,0,0,-1.00003123f,0};
        MeshSelection actual;actual.instance_id=catalog.binding.bed_instance_id;
        actual.resource_id=catalog.binding.bed->id;actual.generation=1;actual.binding=BindingEvidence::Candidate;
        actual.sections={1,2};actual.transform=identity;
        actual.transform[12]=3398;actual.transform[13]=1513.99988f;actual.transform[14]=272;
        captured.meshes.push_back({actual,catalog.binding.bed});
        CHECK(BedroomBedFraming(captured,*catalog.bed_override,bounds));
        CHECK(bounds.right>1.01 && bounds.right<1.03);
        std::cout<<"capture_1084_bed_px="<<bounds.left*1024<<","<<bounds.top*768<<","<<bounds.right*1024<<","<<bounds.bottom*768<<"\n";
        // Candidate left candle reconstructed from the same recorded frame.
        captured.map_epoch=1;captured.device_epoch=2;captured.continuity=3;captured.frame_id=1084;
        captured.provenance.origin=FrameOrigin::Synthetic;captured.provenance.session_id="capture-fixture";
        captured.light_coverage=LightCoverage::Partial;
        RawLightItem recorded;recorded.candidate.id="light-actor:"+map+":"+hash+":D_384:14";
        recorded.candidate.generation=1085;recorded.candidate.binding=BindingEvidence::Candidate;
        recorded.field_34={1163838683u,1154443181u,1135770954u};
        recorded.field_68={1054925026u,1051067744u,1040055032u};recorded.field_8c=1064285172u;
        Vec3 recorded_position{};std::memcpy(recorded_position.data(),recorded.field_34.data(),sizeof(float)*3);
        recorded.candidate.position={recorded_position,BindingEvidence::Candidate};captured.lights={recorded.candidate};
        auto recorded_batch=std::make_shared<RawLightBatch>();recorded_batch->listed=1;
        recorded_batch->items={recorded};recorded_batch->candle_actor_number=14;
        recorded_batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::Candidate;
        recorded_batch->candle_link.factor_8c_bits=recorded.field_8c;
        recorded_batch->candle_link.transform_available=true;
        recorded_batch->candle_link.transform_bits={3212114443u,1049927729u,0u,0u,3197411377u,3212114443u,0u,0u,
                                                    0u,0u,1065353216u,0u,1163838124u,1154443520u,1134757038u,1065353216u};
        RawLightFrame recorded_raw;recorded_raw.map_epoch=1;recorded_raw.device_epoch=2;recorded_raw.continuity=3;
        recorded_raw.frame_id=1084;recorded_raw.provenance=captured.provenance;recorded_raw.camera=captured.camera;
        recorded_raw.batch=recorded_batch;
        CHECK(BedroomCandleFraming(captured,recorded_raw,bounds));
        std::cout<<"capture_1084_candle_px="<<bounds.left*1024<<","<<bounds.top*768<<","<<bounds.right*1024<<","<<bounds.bottom*768<<"\n";
        // Same camera position, theoretical horizontal rotation of about 3 degrees.
        captured.camera.view={.416615465f,-.155418496f,-.895698913f,0,-.909082728f,-.071225360f,-.410481896f,0,
                              0,.985277593f,-.170961872f,0,-2.110205f,332.936529f,3990.632933f,1};
        CHECK(BedroomBedFraming(captured,*catalog.bed_override,bounds));
        CHECK(bounds.left>=.02 && bounds.top>=.02 && bounds.right<=.98 && bounds.bottom<=.98 && bounds.right>.95);
        std::cout<<"rotated_bed_px="<<bounds.left*1024<<","<<bounds.top*768<<","<<bounds.right*1024<<","<<bounds.bottom*768<<"\n";
        recorded_raw.camera=captured.camera;CHECK(BedroomCandleFraming(captured,recorded_raw,bounds));
        CHECK(bounds.left>=.02 && bounds.top>=.02 && bounds.right<=.98 && bounds.bottom<=.98);
        std::cout<<"rotated_candle_px="<<bounds.left*1024<<","<<bounds.top*768<<","<<bounds.right*1024<<","<<bounds.bottom*768<<"\n";

        // Frame 885 camera and two light/visual candidates from the recorded
        // session. The second actor-link sample is reconstructed offline.
        captured.camera.view={.445495993f,-.118446328f,-.887414038f,0,-.895283878f,-.0589392558f,-.441579938f,0,
                              -3.72529008e-9f,.991209567f,-.132300302f,0,-130.278809f,176.359848f,3998.62256f,1};
        captured.frame_id=885;recorded_raw.frame_id=885;recorded_raw.camera=captured.camera;
        recorded.field_8c=1062289552u;recorded.candidate.generation=886;
        recorded_batch->items[0]=recorded;recorded_batch->candle_link.factor_8c_bits=recorded.field_8c;
        auto bedside=recorded;bedside.candidate.id="light-actor:"+map+":"+hash+":D_384:6";
        bedside.field_34={1163246960u,1152505639u,1135036811u};
        std::memcpy(recorded_position.data(),bedside.field_34.data(),sizeof(float)*3);
        bedside.candidate.position={recorded_position,BindingEvidence::Candidate};
        recorded_batch->items.push_back(bedside);recorded_batch->listed=2;
        recorded_batch->other_candle_link=recorded_batch->candle_link;
        recorded_batch->other_candle_link.transform_bits={3212114443u,1049927729u,0u,0u,3197411377u,3212114443u,0u,0u,
                                                          0u,0u,1065353216u,0u,1163246401u,1152505978u,1134022895u,1065353216u};
        captured.lights={recorded.candidate,bedside.candidate};
        CHECK(BedroomBedFraming(captured,*catalog.bed_override,bounds));
        CHECK(bounds.left>=.02 && bounds.top>=.02 && bounds.right<=.98 && bounds.bottom<=.98);
        for (const auto number:{14u,6u}) {
            CHECK(BedroomCandleFraming(captured,recorded_raw,bounds,number));
            CHECK(bounds.left>=.02 && bounds.top>=.02 && bounds.right<=.98 && bounds.bottom<=.98);
            std::cout<<"capture_885_candle_"<<number<<"_px="<<bounds.left*1024<<","<<bounds.top*768<<","<<bounds.right*1024<<","<<bounds.bottom*768<<"\n";
        }
    } else CHECK(argc==1);
    std::cout<<"scene_live_framing=ok\n";
}
