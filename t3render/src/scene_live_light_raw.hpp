#pragma once

// Diagnostic values only. None of these structures belong to SceneIR or
// retain a process address. A sample is attached to a published frame by the
// driver and may be dropped if the frame is superseded.
#include "scene_live_visible_lights.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace t3render::live {
struct RawLightItem {
    VisualLight candidate;
    std::array<std::uint32_t,3> field_24{},field_34{},field_68{};
    std::uint32_t field_58=0,field_5c=0,field_60=0,field_64=0;
    std::uint32_t field_74=0,field_78=0,field_7c=0,field_88=0,field_8c=0,field_90=0;
    bool operator==(const RawLightItem& b) const {
        return candidate==b.candidate && field_24==b.field_24 && field_34==b.field_34 && field_68==b.field_68 &&
            field_58==b.field_58 && field_5c==b.field_5c && field_60==b.field_60 && field_64==b.field_64 &&
            field_74==b.field_74 && field_78==b.field_78 && field_7c==b.field_7c && field_88==b.field_88 &&
            field_8c==b.field_8c && field_90==b.field_90;
    }
};
struct RawLightBatch {
    std::uint32_t listed=0,skipped=0;
    bool truncated=false;
    scene_bridge::CandleLinkObservation candle_link;
    std::vector<RawLightItem> items;
    bool operator==(const RawLightBatch& b) const {
        return listed==b.listed && skipped==b.skipped && truncated==b.truncated && candle_link==b.candle_link && items==b.items;
    }
};
struct RawLightFrame {
    std::uint64_t map_epoch=0,device_epoch=0,frame_id=0,continuity=0;
    FrameProvenance provenance;
    Camera camera;
    std::shared_ptr<const RawLightBatch> batch;
};

// The selected offsets are scalar/float words copied by the statically
// analyzed light descriptor. Their units and semantics remain unverified.
// Unknown +0x80/+0x84 are intentionally not copied into diagnostics.
inline std::shared_ptr<const RawLightBatch> BuildRawLightBatch(
        const scene_bridge::VisibleLightCandidates& source,const std::string& map,const std::string& map_sha256,
        std::uint64_t generation,const std::vector<VisualLight>& lights,
        scene_bridge::CandleLinkObservation candle_link={}) noexcept {
    try {
        std::vector<VisualLight> expected;
        if (!BuildVisibleLightCandidates(source,map,map_sha256,generation,expected) || expected!=lights) return {};
        auto result=std::make_shared<RawLightBatch>();
        result->listed=source.listed;result->skipped=source.skipped;result->truncated=source.truncated;
        result->candle_link=candle_link;
        result->items.reserve(source.count);
        for (std::uint32_t i=0;i<source.count;++i) {
            const auto& words=source.items[i].words;RawLightItem item;item.candidate=lights[i];
            std::copy_n(words.direction_24,3,item.field_24.begin());
            std::copy_n(words.position_34,3,item.field_34.begin());
            std::copy_n(words.words_58_to_90+4,3,item.field_68.begin());
            item.field_58=words.words_58_to_90[0];item.field_5c=words.words_58_to_90[1];
            item.field_60=words.words_58_to_90[2];item.field_64=words.words_58_to_90[3];
            item.field_74=words.words_58_to_90[7];item.field_78=words.words_58_to_90[8];
            item.field_7c=words.words_58_to_90[9];item.field_88=words.words_58_to_90[12];
            item.field_8c=words.words_58_to_90[13];item.field_90=words.words_58_to_90[14];
            result->items.push_back(std::move(item));
        }
        return result;
    } catch (...) {return {};}
}

// Compare by value rather than vector order: SceneWindow sorts by semantic ID.
inline bool RawLightBatchMatches(const RawLightBatch& batch,const std::vector<VisualLight>& lights) {
    using scene_bridge::CandleLinkStatus;
    using scene_bridge::CandleEnabledStatus;
    if ((batch.candle_link.status!=CandleLinkStatus::Unknown && batch.candle_link.status!=CandleLinkStatus::NoLink &&
         batch.candle_link.status!=CandleLinkStatus::Candidate) ||
        (batch.candle_link.status!=CandleLinkStatus::Candidate && batch.candle_link.factor_8c_bits) ||
        (batch.candle_link.status==CandleLinkStatus::Unknown && batch.candle_link.transform_available) ||
        (batch.candle_link.enabled.status!=CandleEnabledStatus::Unknown &&
         batch.candle_link.enabled.status!=CandleEnabledStatus::On && batch.candle_link.enabled.status!=CandleEnabledStatus::Off) ||
        (batch.candle_link.enabled.status==CandleEnabledStatus::Unknown && batch.candle_link.enabled.inherited) ||
        (batch.candle_link.status==CandleLinkStatus::Unknown &&
         batch.candle_link.enabled.status!=CandleEnabledStatus::Unknown) ||
        (!batch.candle_link.transform_available && std::any_of(batch.candle_link.transform_bits.begin(),
             batch.candle_link.transform_bits.end(),[](std::uint32_t word){return word!=0;}))) return false;
    if (batch.items.size()!=lights.size() || batch.items.size()>scene_bridge::kMaxVisibleLightCandidates ||
        batch.listed>scene_bridge::kMaxVisibleLightEntries ||
        batch.items.size()+batch.skipped!=batch.listed) return false;
    for (std::size_t i=0;i<batch.items.size();++i) {
        const auto& item=batch.items[i];const auto& candidate=item.candidate;
        if (candidate.id.empty() || !candidate.generation) return false;
        VisualLight expected;expected.id=candidate.id;expected.generation=candidate.generation;
        expected.binding=BindingEvidence::Candidate;
        Vec3 position{};bool valid=true;
        for (unsigned axis=0;axis<3;++axis) {
            std::memcpy(&position[axis],&item.field_34[axis],sizeof(float));
            valid=valid && std::isfinite(position[axis]) && std::fabs(position[axis])<=1000000;
        }
        if (valid) expected.position={position,BindingEvidence::Candidate};
        if (!(candidate==expected)) return false;
        if (valid) for (unsigned axis=0;axis<3;++axis) {
            std::uint32_t bits=0;std::memcpy(&bits,&(*candidate.position.value)[axis],sizeof(bits));
            if (bits!=item.field_34[axis]) return false;
        }
        unsigned matches=0;
        for (const auto& light:lights) if (light==candidate) {
            bool same=true;
            if (valid) for (unsigned axis=0;axis<3;++axis) {
                std::uint32_t bits=0;std::memcpy(&bits,&(*light.position.value)[axis],sizeof(bits));
                same=same && bits==item.field_34[axis];
            }
            if (same) ++matches;
        }
        if (matches!=1) return false;
        for (std::size_t j=0;j<i;++j) if (batch.items[j].candidate.id==candidate.id) return false;
    }
    return true;
}
inline bool RawLightFrameMatches(const RawLightFrame& raw,const Snapshot& snapshot) {
    return raw.batch && raw.map_epoch==snapshot.map_epoch && raw.device_epoch==snapshot.device_epoch &&
        raw.frame_id==snapshot.frame_id && raw.continuity==snapshot.continuity &&
        raw.provenance.origin==snapshot.provenance.origin && raw.provenance.session_id==snapshot.provenance.session_id &&
        raw.provenance.capture_sha256==snapshot.provenance.capture_sha256 &&
        std::memcmp(raw.camera.view.data(),snapshot.camera.view.data(),sizeof(float)*raw.camera.view.size())==0 &&
        std::memcmp(raw.camera.projection.data(),snapshot.camera.projection.data(),sizeof(float)*raw.camera.projection.size())==0 &&
        raw.camera.x==snapshot.camera.x && raw.camera.y==snapshot.camera.y && raw.camera.width==snapshot.camera.width &&
        raw.camera.height==snapshot.camera.height && raw.camera.viewport_source==snapshot.camera.viewport_source &&
        std::memcmp(&raw.camera.min_depth,&snapshot.camera.min_depth,sizeof(float))==0 &&
        std::memcmp(&raw.camera.max_depth,&snapshot.camera.max_depth,sizeof(float))==0 &&
        snapshot.light_coverage==LightCoverage::Partial && RawLightBatchMatches(*raw.batch,snapshot.lights);
}
} // namespace t3render::live
