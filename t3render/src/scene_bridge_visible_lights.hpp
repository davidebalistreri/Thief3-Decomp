#pragma once

#include "scene_bridge_light_candidate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace t3render::scene_bridge {

// The renderer's +0x74 collection is a bounded observation, not a promise of
// all lights affecting a frame. Never serialize the temporary addresses.
constexpr std::size_t kMaxVisibleLightEntries = 128;
constexpr std::size_t kMaxVisibleLightCandidates = 32;
struct VisibleLightCandidate {
    char actor_name[64]{};
    std::uint32_t actor_number = 0; // FName number; zero means no suffix
    LightCandidateWords words{};
};
struct VisibleLightCandidates {
    bool stable = false, truncated = false;
    std::uint32_t listed = 0, skipped = 0, count = 0;
    VisibleLightCandidate items[kMaxVisibleLightCandidates]{};
};

namespace visible_light_detail {
struct Array { std::uint32_t count = 0, capacity_bytes = 0, data = 0; };
inline bool Same(const Array& a, const Array& b) noexcept {
    return a.count==b.count && a.capacity_bytes==b.capacity_bytes && a.data==b.data;
}
inline bool EqualAscii(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        auto x=static_cast<unsigned char>(*a++), y=static_cast<unsigned char>(*b++);
        if (x>='A' && x<='Z') x=static_cast<unsigned char>(x+'a'-'A');
        if (y>='A' && y<='Z') y=static_cast<unsigned char>(y+'a'-'A');
        if (x!=y) return false;
    }
    return !*a && !*b;
}
inline bool MapMatches(const char* name, const char* map) noexcept {
    constexpr char prefix[]="LOADGAME:";
    std::size_t i=0;
    while (i<sizeof(prefix)-1 && name[i] &&
           ((name[i]>='a' && name[i]<='z' ? name[i]-'a'+'A' : name[i])==prefix[i])) ++i;
    return EqualAscii(i==sizeof(prefix)-1 ? name+i : name,map);
}
inline ProbeNamedObject ReadMember(const ProbeReader& reader,const ProbeArray32& table,
                                   std::uint32_t address) noexcept {
    const auto index=ReadProbeOffset(reader,address,0x04);
    return index.available ? ReadNamedProbeObjectAtSlot(reader,table,{address,true},index.value) : ProbeNamedObject{};
}
} // namespace visible_light_detail

// Call inside an independently bracketed selected-pass Present interval.
// The caller must validate this build and renderer pointer. Unknown/unsupported
// entries are skipped, so even stable=true never means complete coverage.
inline VisibleLightCandidates SampleVisibleLightCandidates(const ProbeReader& reader,
                                                           std::uint32_t renderer,const char* map) noexcept {
    using namespace visible_light_detail;
    VisibleLightCandidates result;
    if (!reader.read || !renderer || !map || !*map) return result;
    Array first{},last{};
    if (!ReadProbeBytesOffset(reader,renderer,0x74,&first,sizeof(first)) ||
        first.count>first.capacity_bytes/4 || first.capacity_bytes%4 || first.capacity_bytes>4*65536 ||
        (first.count && !first.data)) return result;
    result.listed=first.count;
    if (first.count>kMaxVisibleLightEntries) {result.truncated=true;return result;}
    std::uint32_t entries[kMaxVisibleLightEntries]{}, after[kMaxVisibleLightEntries]{};
    const auto bytes=first.count*sizeof(entries[0]);
    if (bytes && !ReadProbe(reader,first.data,entries,bytes)) return result;
    ProbeArray32 objects{},objects_after{};
    if (!ReadProbe(reader,0x10f3e4a0,&objects,sizeof(objects)) || !ValidProbeArray(objects,65536)) return result;
    for (std::uint32_t i=0;i<first.count;++i) {
        const auto light=entries[i];
        if (!light || std::find(entries,entries+i,light)!=entries+i) {++result.skipped;continue;}
        const auto vtable=ReadProbeU32(reader,light);
        const auto owner=ReadProbeOffset(reader,light,0x54);
        if (!vtable.available || vtable.value!=light_candidate_detail::kLightVtable || !owner.available || !owner.value) {
            ++result.skipped;continue;
        }
        const auto actor=ReadMember(reader,objects,owner.value);
        if (!actor.member || !actor.stable || !actor.name_available || !actor.fname.available ||
            !actor.outer.available || !actor.outer.value || std::strchr(actor.name,':')) {++result.skipped;continue;}
        const auto outer=ReadMember(reader,objects,actor.outer.value);
        if (!outer.member || !outer.stable || !outer.name_available || !outer.fname.available ||
            (outer.fname.value>>16)!=0 || !outer.outer.available || outer.outer.value ||
            !MapMatches(outer.name,map)) {++result.skipped;continue;}
        const auto link=ReadProbeOffset(reader,owner.value,0xb8);
        if (!link.available || link.value!=light) {++result.skipped;continue;}
        const auto sample=InspectLightCandidate(reader,owner.value);
        const auto link_after=ReadProbeOffset(reader,owner.value,0xb8);
        if (sample.status!=LightCandidateStatus::Candidate || !link_after.available || link_after.value!=light) {
            ++result.skipped;continue;
        }
        const auto actor_after=ReadMember(reader,objects,owner.value);
        const auto outer_after=ReadMember(reader,objects,actor.outer.value);
        if (!actor_after.member || !actor_after.stable || !actor_after.name_available ||
            !actor_after.fname.available || actor_after.fname.value!=actor.fname.value ||
            !actor_after.outer.available || actor_after.outer.value!=actor.outer.value ||
            std::strcmp(actor.name,actor_after.name)!=0 || !outer_after.member || !outer_after.stable ||
            !outer_after.name_available || !outer_after.fname.available || outer_after.fname.value!=outer.fname.value ||
            !outer_after.outer.available || outer_after.outer.value!=outer.outer.value ||
            std::strcmp(outer.name,outer_after.name)!=0) {++result.skipped;continue;}
        const auto number=actor.fname.value>>16;
        for (std::uint32_t j=0;j<result.count;++j)
            if (result.items[j].actor_number==number && EqualAscii(result.items[j].actor_name,actor.name))
                return VisibleLightCandidates{}; // ambiguous semantic identity
        if (result.count==kMaxVisibleLightCandidates) {result.truncated=true;++result.skipped;continue;}
        auto& item=result.items[result.count++];
        std::memcpy(item.actor_name,actor.name,sizeof(item.actor_name));item.actor_number=number;item.words=sample.words;
    }
    if (!ReadProbeBytesOffset(reader,renderer,0x74,&last,sizeof(last)) || !Same(first,last) ||
        (bytes && (!ReadProbe(reader,first.data,after,bytes) || std::memcmp(entries,after,bytes))) ||
        !ReadProbe(reader,0x10f3e4a0,&objects_after,sizeof(objects_after)) || !SameProbeArray(objects,objects_after))
        return VisibleLightCandidates{};
    result.stable=true;
    return result;
}
} // namespace t3render::scene_bridge
