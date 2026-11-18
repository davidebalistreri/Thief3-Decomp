#include "../src/scene_live_lighting.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace t3render::live;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return 1; } } while (0)

int main() {
    const std::string map="SouthQuarter1_int1";
    const std::string hash="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
    const std::string id="light-actor:"+map+":"+hash+":D_384:6";
    Snapshot s;s.map=map;s.map_sha256=hash;s.map_epoch=1;s.device_epoch=2;s.continuity=3;s.frame_id=4;
    s.provenance.origin=FrameOrigin::Synthetic;s.provenance.session_id="fixture";
    s.light_coverage=LightCoverage::Partial;
    VisualLight light;light.id=id;light.generation=5;light.binding=BindingEvidence::Candidate;
    light.position={Vec3{1,2,3},BindingEvidence::Candidate};s.lights.push_back(light);
    RawLightItem item;item.candidate=light;
    const float position[3]={1,2,3},color[3]={0.4f,0.2f,0.1f};
    std::memcpy(item.field_34.data(),position,sizeof(position));
    std::memcpy(item.field_68.data(),color,sizeof(color));
    const float full=1;std::memcpy(&item.field_8c,&full,sizeof(full));
    auto batch=std::make_shared<RawLightBatch>();batch->listed=1;batch->items.push_back(item);
    RawLightFrame raw;raw.map_epoch=1;raw.device_epoch=2;raw.continuity=3;raw.frame_id=4;
    raw.provenance=s.provenance;raw.camera=s.camera;raw.batch=batch;
    RenderLight out;CHECK(CandlePreviewFromRaw(s,raw,out));
    CHECK(out.position==*light.position.value && out.id==id && out.generation==5);
    CHECK(std::fabs(out.color_linear[0]-1)<1e-6f && std::fabs(out.color_linear[1]-0.5f)<1e-6f &&
          std::fabs(out.color_linear[2]-0.25f)<1e-6f);
    CHECK(out.intensity==1);
    s.conventions.length_unit=LengthUnit::T3Unit;s.conventions.axes=Axes::T3Xyz;s.conventions.up_axis=UpAxis::Z;
    s.conventions.meters_per_unit=0.01905f;
    batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::Candidate;
    batch->candle_link.factor_8c_bits=batch->items[0].field_8c;
    batch->candle_link.transform_available=true;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::memcpy(batch->candle_link.transform_bits.data(),identity,sizeof(identity));
    std::array<ResidentFlatDraw,2> candlestick;
    CHECK(CandlePreviewCandlestick(s,raw,candlestick));
    CHECK(candlestick[0].resource && candlestick[0].resource==candlestick[1].resource &&
          candlestick[0].index_count==3952*3 && candlestick[1].index_count==376*3);
    ++raw.frame_id;CHECK(!CandlePreviewCandlestick(s,raw,candlestick));--raw.frame_id;
    s.map_sha256=std::string(64,'a');CHECK(!CandlePreviewCandlestick(s,raw,candlestick));s.map_sha256=hash;
    const float invalid_scale=std::numeric_limits<float>::quiet_NaN();s.conventions.meters_per_unit=invalid_scale;
    CHECK(!CandlePreviewCandlestick(s,raw,candlestick));s.conventions.meters_per_unit=0.01905f;
    const float huge_scale=100;std::memcpy(&batch->candle_link.transform_bits[0],&huge_scale,sizeof(huge_scale));
    CHECK(!CandlePreviewCandlestick(s,raw,candlestick));
    std::memcpy(batch->candle_link.transform_bits.data(),identity,sizeof(identity));
    ResidentFlatDraw flame;CHECK(CandlePreviewFlame(s,raw,flame));
    CHECK(flame.resource && flame.index_count==744 && flame.emission==8 && flame.transform[15]==1);
    ++raw.frame_id;CHECK(!CandlePreviewFlame(s,raw,flame));--raw.frame_id;
    batch->candle_link.factor_8c_bits=0;
    CHECK(!CandlePreviewFlame(s,raw,flame) && !CandlePreviewFromRaw(s,raw,out));
    batch->candle_link.factor_8c_bits=batch->items[0].field_8c;
    batch->items[0].field_8c=0;batch->candle_link.factor_8c_bits=0;
    CHECK(!CandlePreviewFlame(s,raw,flame));batch->items[0]=item;
    batch->candle_link.factor_8c_bits=batch->items[0].field_8c;
    batch->candle_link.transform_bits[15]=0;CHECK(!CandlePreviewFlame(s,raw,flame));
    std::memcpy(batch->candle_link.transform_bits.data(),identity,sizeof(identity));
    batch->candle_link.transform_available=false;batch->candle_link.transform_bits={};
    CHECK(!CandlePreviewFlame(s,raw,flame));
    batch->candle_link={};
    VerifiedLighting verified;CHECK(BuildVerifiedLighting(s,verified)==LightingResult::UnknownCoverage);
    const RenderLight good=out;
    ++raw.frame_id;CHECK(!CandlePreviewFromRaw(s,raw,out));CHECK(out.id==good.id);--raw.frame_id;
    raw.camera.view[12]=1;CHECK(!CandlePreviewFromRaw(s,raw,out));raw.camera=s.camera;
    s.map_sha256=std::string(64,'a');CHECK(!CandlePreviewFromRaw(s,raw,out));s.map_sha256=hash;
    batch->items[0].candidate.id+="wrong";CHECK(!CandlePreviewFromRaw(s,raw,out));batch->items[0]=item;
    const float half=.5f,zero=0,over=1.1f;
    std::memcpy(&batch->items[0].field_8c,&half,sizeof(half));CHECK(CandlePreviewFromRaw(s,raw,out) && out.intensity==.5f);
    std::memcpy(&batch->items[0].field_8c,&zero,sizeof(zero));CHECK(CandlePreviewFromRaw(s,raw,out) && out.intensity==0);
    std::memcpy(&batch->items[0].field_8c,&over,sizeof(over));CHECK(!CandlePreviewFromRaw(s,raw,out));
    batch->items[0]=item;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    std::memcpy(&batch->items[0].field_68[1],&nan,sizeof(nan));CHECK(!CandlePreviewFromRaw(s,raw,out));
    batch->items[0]=item;std::memcpy(&batch->items[0].field_8c,&nan,sizeof(nan));CHECK(!CandlePreviewFromRaw(s,raw,out));
    batch->items[0]=item;batch->items[0].field_34[0]=0;CHECK(!CandlePreviewFromRaw(s,raw,out));
    batch->items.clear();batch->listed=0;s.lights.clear();
    batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::Candidate;
    batch->candle_link.factor_8c_bits=0;
    CHECK(CandlePreviewDirectZero(s,raw));
    batch->candle_link.transform_available=true;
    std::memcpy(batch->candle_link.transform_bits.data(),identity,sizeof(identity));
    CHECK(CandlePreviewCandlestick(s,raw,candlestick));CHECK(!CandlePreviewFlame(s,raw,flame));
    batch->candle_link.transform_available=false;batch->candle_link.transform_bits={};
    ++raw.frame_id;CHECK(!CandlePreviewDirectZero(s,raw));--raw.frame_id;
    raw.camera.view[12]=1;CHECK(!CandlePreviewDirectZero(s,raw));raw.camera=s.camera;
    batch->candle_link.factor_8c_bits=0x3f800000;CHECK(!CandlePreviewDirectZero(s,raw));
    batch->candle_link.factor_8c_bits=0;batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::NoLink;
    batch->candle_link.transform_available=true;
    std::memcpy(batch->candle_link.transform_bits.data(),identity,sizeof(identity));
    CHECK(CandlePreviewCandlestick(s,raw,candlestick) && !CandlePreviewFlame(s,raw,flame));
    CHECK(!CandlePreviewDirectZero(s,raw));
    ++raw.frame_id;CHECK(!CandlePreviewCandlestick(s,raw,candlestick));--raw.frame_id;
    batch->items.push_back(item);batch->listed=1;s.lights.push_back(light);
    CHECK(!CandlePreviewFromRaw(s,raw,out) && !CandlePreviewFlame(s,raw,flame));
    batch->items.clear();batch->listed=0;s.lights.clear();
    batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::Unknown;
    CHECK(!CandlePreviewCandlestick(s,raw,candlestick));
    batch->candle_link.transform_available=false;batch->candle_link.transform_bits={};
    CHECK(!CandlePreviewDirectZero(s,raw));
    batch->candle_link.status=t3render::scene_bridge::CandleLinkStatus::Candidate;
    batch->items.push_back(item);batch->listed=1;s.lights.push_back(light);
    CHECK(!CandlePreviewDirectZero(s,raw));
    raw.batch.reset();CHECK(!CandlePreviewFlame(s,raw,flame));
    std::cout << "candle_preview=ok\n";
}
