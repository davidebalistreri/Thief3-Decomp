#pragma once

#include "scene_live_light_diagnostics.hpp"
#include <cstring>

namespace t3render::live {

// Written only for requested comparison images. This records the viewer's
// published selection, not GPU bindings or proof of visible pixels.
inline void WriteSceneSelection(std::ostream& out,const Snapshot& snapshot) {
    constexpr std::size_t limit=4096;
    const auto scope=[](IdentityScope value) {
        return value==IdentityScope::VisibleList ? "visible_list" :
               value==IdentityScope::FullObjectScan ? "full_object_scan" : "unknown";
    };
    const auto sections=[&out](const std::vector<std::uint32_t>& values) {
        out<<'[';
        for (std::size_t i=0;i<values.size();++i) {if (i) out<<',';out<<values[i];}
        out<<']';
    };
    out<<",\"scene_selection\":{\"truncated\":"
       <<(snapshot.meshes.size()>limit || snapshot.bsp.size()>limit ? "true" : "false")<<",\"meshes\":[";
    for (std::size_t i=0;i<std::min(snapshot.meshes.size(),limit);++i) {
        if (i) out<<',';
        const auto& item=snapshot.meshes[i].selection;
        out<<"{\"id\":";light_diagnostic_detail::JsonString(out,item.instance_id);
        out<<",\"resource_id\":";light_diagnostic_detail::JsonString(out,item.resource_id);
        out<<",\"generation\":"<<item.generation<<",\"binding\":\""
           <<light_diagnostic_detail::Evidence(item.binding)<<"\",\"scope\":\""<<scope(item.identity_scope)
           <<"\",\"transform_bits\":[";
        for (std::size_t j=0;j<item.transform.size();++j) {
            std::uint32_t bits=0;std::memcpy(&bits,&item.transform[j],sizeof(bits));
            if (j) out<<',';
            out<<bits;
        }
        out<<"],\"sections\":";sections(item.sections);out<<'}';
    }
    out<<"],\"bsp\":[";
    for (std::size_t i=0;i<std::min(snapshot.bsp.size(),limit);++i) {
        if (i) out<<',';
        const auto& item=snapshot.bsp[i].selection;
        out<<"{\"id\":";light_diagnostic_detail::JsonString(out,item.resource_id);
        out<<",\"binding\":\""<<light_diagnostic_detail::Evidence(item.binding)
           <<"\",\"scope\":\""<<scope(item.identity_scope)<<"\",\"sections\":";
        sections(item.sections);out<<'}';
    }
    out<<"]}";
}
} // namespace t3render::live
