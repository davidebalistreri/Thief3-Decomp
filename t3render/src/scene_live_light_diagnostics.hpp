#pragma once

#include "scene_live_light_raw.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

namespace t3render::live {
namespace light_diagnostic_detail {
inline void JsonString(std::ostream& out,const std::string& value) {
    constexpr char hex[]="0123456789abcdef";
    out<<'"';
    for (std::size_t i=0;i<value.size();++i) {
        const auto c=static_cast<unsigned char>(value[i]);
        if (c=='"' || c=='\\') out<<'\\'<<static_cast<char>(c);
        else if (c<0x20) out<<"\\u00"<<hex[c>>4]<<hex[c&15];
        else if (c>=0x80) {
            const std::size_t n=c>=0xc2 && c<=0xdf ? 2 : c>=0xe0 && c<=0xef ? 3 : c>=0xf0 && c<=0xf4 ? 4 : 0;
            bool valid=n && i+n<=value.size();
            for (std::size_t j=1;valid && j<n;++j)
                valid=(static_cast<unsigned char>(value[i+j])&0xc0)==0x80;
            if (valid && n>=3) {
                const auto next=static_cast<unsigned char>(value[i+1]);
                valid=!(c==0xe0 && next<0xa0) && !(c==0xed && next>=0xa0) &&
                      !(c==0xf0 && next<0x90) && !(c==0xf4 && next>=0x90);
            }
            if (valid) {out.write(value.data()+i,static_cast<std::streamsize>(n));i+=n-1;}
            else out<<"\\u00"<<hex[c>>4]<<hex[c&15];
        }
        else out<<static_cast<char>(c);
    }
    out<<'"';
}
inline const char* Evidence(BindingEvidence value) {
    return value==BindingEvidence::Verified ? "verified" : value==BindingEvidence::Candidate ? "candidate" : "unknown";
}
} // namespace light_diagnostic_detail

// Record a per-frame session identity so raw values cannot be mistaken for a
// different process session or a captured replay with the same frame number.
inline void WriteFrameIdentity(std::ostream& out,const Snapshot& snapshot) {
    const char* source=snapshot.provenance.origin==FrameOrigin::LiveProcess ? "live_process" :
                       snapshot.provenance.origin==FrameOrigin::CapturedReplay ? "captured_replay" :
                       snapshot.provenance.origin==FrameOrigin::Synthetic ? "synthetic" : "unknown";
    out<<",\"source\":\""<<source<<"\",\"session_id\":";
    light_diagnostic_detail::JsonString(out,snapshot.provenance.session_id);
    out<<",\"frame\":"<<snapshot.frame_id<<",\"map\":";
    light_diagnostic_detail::JsonString(out,snapshot.map);
    out<<",\"map_sha256\":";light_diagnostic_detail::JsonString(out,snapshot.map_sha256);
    if (snapshot.provenance.capture_sha256) {
        out<<",\"capture_sha256\":";light_diagnostic_detail::JsonString(out,*snapshot.provenance.capture_sha256);
    }
}

// Append bounded details to an existing frame JSON record. The scene has
// already validated finite positions; no engine addresses or raw words leave.
inline void WriteLightDetails(std::ostream& out,const std::vector<VisualLight>& lights) {
    constexpr std::size_t limit=32;
    out<<",\"light_details_truncated\":"<<(lights.size()>limit ? "true" : "false")<<",\"light_details\":[";
    for (std::size_t i=0;i<std::min(lights.size(),limit);++i) {
        if (i) out<<',';
        const auto& light=lights[i];out<<"{\"id\":";light_diagnostic_detail::JsonString(out,light.id);
        out<<",\"generation\":"<<light.generation<<",\"binding\":\""<<light_diagnostic_detail::Evidence(light.binding)
           <<"\",\"position_evidence\":\""<<light_diagnostic_detail::Evidence(light.position.evidence)<<"\",\"position\":";
        if (!light.position.value) out<<"null";
        else out<<'['<<(*light.position.value)[0]<<','<<(*light.position.value)[1]<<','<<(*light.position.value)[2]<<']';
        out<<'}';
    }
    out<<']';
}

// The optional raw record must match frame metadata and candidate values.
// The producer separately attests the camera/Present interval. Integer words
// preserve bits; they do not assert physical interpretation.
inline void WriteRawLightDetails(std::ostream& out,const Snapshot& snapshot,const RawLightFrame* raw) {
    if (!raw || !RawLightFrameMatches(*raw,snapshot)) {out<<",\"light_raw_available\":false";return;}
    const auto& batch=*raw->batch;
    out<<",\"light_raw_available\":true,\"light_raw\":{\"listed\":"<<batch.listed
       <<",\"skipped\":"<<batch.skipped<<",\"truncated\":"<<(batch.truncated ? "true" : "false")
       <<",\"candle_link\":{\"status\":\""
       <<(batch.candle_link.status==scene_bridge::CandleLinkStatus::Candidate ? "candidate" :
          batch.candle_link.status==scene_bridge::CandleLinkStatus::NoLink ? "no_link" : "unknown")
       <<"\",\"field_8c_bits\":";
    if (batch.candle_link.status==scene_bridge::CandleLinkStatus::Candidate) out<<batch.candle_link.factor_8c_bits;
    else out<<"null";
    out<<",\"enabled_candidate\":";
    if (batch.candle_link.enabled.status==scene_bridge::CandleEnabledStatus::On) out<<"true";
    else if (batch.candle_link.enabled.status==scene_bridge::CandleEnabledStatus::Off) out<<"false";
    else out<<"null";
    out<<",\"enabled_inherited\":";
    if (batch.candle_link.enabled.status==scene_bridge::CandleEnabledStatus::Unknown) out<<"null";
    else out<<(batch.candle_link.enabled.inherited ? "true" : "false");
    out<<",\"transform_bits\":";
    if (batch.candle_link.transform_available) {
        out<<'[';
        for (std::size_t i=0;i<batch.candle_link.transform_bits.size();++i) {
            if (i) out<<',';
            out<<batch.candle_link.transform_bits[i];
        }
        out<<']';
    } else out<<"null";
    out<<"},\"items\":[";
    for (std::size_t i=0;i<batch.items.size();++i) {
        if (i) out<<',';
        const auto& item=batch.items[i];out<<"{\"id\":";light_diagnostic_detail::JsonString(out,item.candidate.id);
        out<<",\"generation\":"<<item.candidate.generation
           <<",\"field_24_bits\":["<<item.field_24[0]<<','<<item.field_24[1]<<','<<item.field_24[2]<<']'
           <<",\"field_34_bits\":["<<item.field_34[0]<<','<<item.field_34[1]<<','<<item.field_34[2]<<']'
           <<",\"field_58\":"<<item.field_58<<",\"field_5c\":"<<item.field_5c<<",\"field_60\":"<<item.field_60
           <<",\"field_64\":"<<item.field_64
           <<",\"field_68_bits\":["<<item.field_68[0]<<','<<item.field_68[1]<<','<<item.field_68[2]<<']'
           <<",\"field_74_bits\":"<<item.field_74<<",\"field_78_bits\":"<<item.field_78
           <<",\"field_7c_bits\":"<<item.field_7c<<",\"field_88_bits\":"<<item.field_88
           <<",\"field_8c_bits\":"<<item.field_8c<<",\"field_90_bits\":"<<item.field_90<<'}';
    }
    out<<"]}";
}
} // namespace t3render::live
