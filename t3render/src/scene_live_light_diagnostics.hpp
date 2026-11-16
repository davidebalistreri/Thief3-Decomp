#pragma once

#include "../ir/scene_live.hpp"

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
} // namespace t3render::live
