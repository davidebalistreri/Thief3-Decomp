#pragma once

#include "scene_bridge_candle_enabled.hpp"
#include "scene_bridge_light_candidate.hpp"

#include <algorithm>
#include <array>
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

// A separate diagnostic for the one bedroom actor. Absence, unreadable
// memory and ambiguity remain Unknown; NoLink does not mean bLightOn=false.
enum class CandleLinkStatus { Unknown, NoLink, Candidate };
struct CandleLinkObservation {
    CandleLinkStatus status = CandleLinkStatus::Unknown;
    std::uint32_t factor_8c_bits = 0;
    CandleEnabledObservation enabled;
    bool transform_available = false;
    std::array<std::uint32_t,16> transform_bits{};
    bool operator==(const CandleLinkObservation& other) const noexcept {
        return status==other.status && factor_8c_bits==other.factor_8c_bits &&
               enabled.status==other.enabled.status && enabled.inherited==other.enabled.inherited &&
               transform_available==other.transform_available && transform_bits==other.transform_bits;
    }
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

// Find either researched bedroom candlestick through the current visual list rather than the renderer's
// partial light list. The caller brackets this in the selected Present
// interval and checks the executable and catalog fingerprint. No pointer is
// cached or returned. The list and selected identity are rechecked; these
// checks cannot eliminate ABA or establish global actor-name uniqueness.
inline CandleLinkObservation SampleBedroomCandleLink(const ProbeReader& reader,
                                                      const ProbeSnapshot& snapshot,const char* map,
                                                      std::uint32_t actor_number=6) noexcept {
    using namespace visible_light_detail;
    CandleLinkObservation unknown;
    if (!reader.read || !map || !EqualAscii(map,"SouthQuarter1_int1") || (actor_number!=6 && actor_number!=14) || !snapshot.self ||
        !snapshot.count.available || !snapshot.array.available || !snapshot.array.value ||
        snapshot.entries_truncated || snapshot.count.value!=snapshot.entries_attempted ||
        snapshot.entries_attempted>kMaxProbeEntries) return unknown;
    std::uint32_t selected=0,owner=0,matches=0;
    for (std::uint32_t i=0;i<snapshot.entries_attempted;++i) {
        const auto& entry=snapshot.entries[i];
        if (entry.list_index!=i || !entry.address.available) return unknown;
        if (!entry.address.value) continue;
        if (!entry.vtable.available || !entry.type.available) return unknown;
        if (entry.vtable.value!=0x10e4e8c0 || entry.type.value!=4) continue;
        const auto item_owner=ReadProbeOffset(reader,entry.address.value,0x54);
        if (!item_owner.available || !item_owner.value) return unknown;
        const auto fname=ReadProbeOffset(reader,item_owner.value,0x20);
        if (!fname.available) return unknown;
        if ((fname.value>>16)!=actor_number) continue;
        char name[64]{};
        if (!ReadProbeName(reader,fname.value,name)) return unknown;
        if (std::strcmp(name,"D_384")==0) {selected=i;owner=item_owner.value;++matches;}
    }
    if (matches!=1) return unknown;
    const auto& entry=snapshot.entries[selected];
    ProbeArray32 table{},after{};
    if (!ReadProbe(reader,0x10f3e4a0,&table,sizeof(table)) || !ValidProbeArray(table,65536)) return unknown;
    const auto actor=ReadMember(reader,table,owner);
    if (!actor.member || !actor.stable || !actor.name_available || std::strcmp(actor.name,"D_384") ||
        !actor.fname.available || (actor.fname.value>>16)!=actor_number || !actor.outer.available || !actor.outer.value)
        return unknown;
    const auto outer=ReadMember(reader,table,actor.outer.value);
    if (!outer.member || !outer.stable || !outer.name_available || !outer.fname.available ||
        (outer.fname.value>>16)!=0 || !outer.outer.available || outer.outer.value || !MapMatches(outer.name,map))
        return unknown;
    const auto backlink=ReadProbeOffset(reader,owner,0xb4);
    if (!backlink.available || backlink.value!=entry.address.value) return unknown;
    std::array<std::uint32_t,16> transform{},transform_after{};
    const bool transform_read=ReadProbeBytesOffset(reader,entry.address.value,0x04,transform.data(),sizeof(transform));
    const auto light=InspectLightCandidate(reader,owner);
    if (light.status!=LightCandidateStatus::Candidate && light.status!=LightCandidateStatus::NoLight) return unknown;
    const auto enabled=SampleBedroomCandleEnabled(reader,owner);
    if (light.status==LightCandidateStatus::NoLight) {
        const auto link1=ReadProbeOffset(reader,owner,0xb8),link2=ReadProbeOffset(reader,owner,0xb8);
        if (!link1.available || link1.value || !link2.available || link2.value) return unknown;
    }
    const auto actor_after=ReadMember(reader,table,owner),outer_after=ReadMember(reader,table,actor.outer.value);
    const auto back_after=ReadProbeOffset(reader,owner,0xb4),element_owner=ReadProbeOffset(reader,entry.address.value,0x54);
    const auto vtable=ReadProbeU32(reader,entry.address.value),type=ReadProbeOffset(reader,entry.address.value,0x44);
    const auto count=ReadProbeOffset(reader,snapshot.self,0x50),array=ReadProbeOffset(reader,snapshot.self,0x58);
    std::uint32_t slots[kMaxProbeEntries]{};
    if (!SameNamedProbeObject(actor,actor_after) || !SameNamedProbeObject(outer,outer_after) ||
        !back_after.available || back_after.value!=entry.address.value ||
        !element_owner.available || element_owner.value!=owner || !vtable.available || vtable.value!=entry.vtable.value ||
        !type.available || type.value!=entry.type.value || !count.available || count.value!=snapshot.count.value ||
        !array.available || array.value!=snapshot.array.value ||
        (snapshot.entries_attempted && !ReadProbe(reader,snapshot.array.value,slots,snapshot.entries_attempted*4)) ||
        !ReadProbe(reader,0x10f3e4a0,&after,sizeof(after)) || !SameProbeArray(table,after)) return unknown;
    for (std::uint32_t i=0;i<snapshot.entries_attempted;++i)
        if (slots[i]!=snapshot.entries[i].address.value) return unknown;
    if (light.status==LightCandidateStatus::NoLight) {
        const auto link=ReadProbeOffset(reader,owner,0xb8);
        if (!link.available || link.value) return unknown;
    }
    CandleLinkObservation result;
    result.enabled=enabled;
    if (light.status==LightCandidateStatus::NoLight) result.status=CandleLinkStatus::NoLink;
    else {
        result.status=CandleLinkStatus::Candidate;result.factor_8c_bits=light.words.words_58_to_90[13];
    }
    // The visual actor can remain visible without an associated light object.
    // Preserve its independently stable transform in either known link state.
    if (transform_read && ReadProbeBytesOffset(reader,entry.address.value,0x04,transform_after.data(),sizeof(transform_after)) &&
        transform==transform_after) {result.transform_available=true;result.transform_bits=transform;}
    return result;
}
} // namespace t3render::scene_bridge
