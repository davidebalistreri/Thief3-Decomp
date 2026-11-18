#pragma once

#include "scene_bridge_snapshot.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace t3render::scene_bridge {

// Candidate only. The caller must validate the executable, bedroom, actor
// identity, and Present interval separately. No transient addresses escape.
enum class CandleEnabledStatus { Unknown, On, Off };
struct CandleEnabledObservation {
    CandleEnabledStatus status = CandleEnabledStatus::Unknown;
    bool inherited = false;
};

namespace candle_enabled_detail {
constexpr std::uint32_t kPropertyId = 0x5cf;
constexpr std::uint32_t kObjectTable = 0x10f3e4a0;
constexpr std::uint32_t kDescriptorTable = 0x10f3eda4;
constexpr std::uint32_t kSlotPool = 0x10f7b5c8;
constexpr std::uint32_t kSlotCount = 61440;
constexpr std::size_t kMaxSlots = 128, kMaxObjects = 16, kMaxClasses = 64;
constexpr std::size_t kMaxReads = 512;

// Record every value that affects the result; compare two complete traversals.
// This detects observed mutations, but cannot eliminate ABA or later changes.
struct Witness { std::uint64_t value = 0; std::uint32_t address = 0; std::uint8_t size = 0; };
struct Sample {
    std::array<Witness,kMaxReads> reads{};
    std::size_t count = 0, cursor = 0;
    bool verifying = false;
};

template<class T> inline bool Read(const ProbeReader& reader, Sample& sample,
                                  std::uint32_t address, T& output) noexcept {
    static_assert(std::is_integral_v<T> && sizeof(T)<=sizeof(std::uint64_t));
    if (!ReadProbe(reader,address,&output,sizeof(output))) return false;
    if (sample.verifying) {
        if (sample.cursor>=sample.count) return false;
        const auto& prior=sample.reads[sample.cursor++];
        return prior.address==address && prior.size==sizeof(output) && prior.value==static_cast<std::uint64_t>(output);
    }
    if (sample.count==sample.reads.size()) return false;
    auto& read=sample.reads[sample.count++];
    read.address=address;read.size=sizeof(output);read.value=static_cast<std::uint64_t>(output);
    return true;
}

template<class T> inline bool At(const ProbeReader& reader, Sample& sample,
                                std::uint32_t base, std::uint32_t offset, T& output) noexcept {
    std::uint32_t address=0;
    return ProbeOffset(base,offset,address) && Read(reader,sample,address,output);
}

inline bool Member(const ProbeReader& reader,Sample& sample,const ProbeArray32& table,
                   std::uint32_t object) noexcept {
    std::uint32_t index=0,slot=0,vtable=0,fname=0,outer=0;
    return object && At(reader,sample,object,0x04,index) && index<table.count &&
        At(reader,sample,table.data,index*4,slot) && slot==object &&
        At(reader,sample,object,0,vtable) && vtable &&
        At(reader,sample,object,0x20,fname) && At(reader,sample,object,0x18,outer);
}

inline bool Descriptor(const ProbeReader& reader,Sample& sample,std::uint32_t& mask) noexcept {
    std::uint32_t table=0,array=0,descriptor=0,vtable=0,copy_all=0,copy_one=0,count=0,size=0;
    return Read(reader,sample,kDescriptorTable,table) && At(reader,sample,table,8,array) &&
        At(reader,sample,array,kPropertyId*4,descriptor) && At(reader,sample,descriptor,0,vtable) &&
        At(reader,sample,vtable,0xb4,copy_all) && At(reader,sample,vtable,0xb0,copy_one) &&
        At(reader,sample,descriptor,0x34,count) && At(reader,sample,descriptor,0x38,size) &&
        At(reader,sample,descriptor,0x60,mask) && vtable==0x10e4e7e0 &&
        copy_all==0x10aecb20 && copy_one==0x10aed570 && count==1 && size==4 && mask;
}

// Static resolver/getter evidence: ev_2367fc2a425b7375649886e01bf35e75864e7fa87ab6237889ece73ba1e4e129
// and ev_cf800960c99d852354cf31dab2d8d069a7e10e61db3b4d2c7e817bd32e8c8753.
// This mirrors the resolver only for 0x008005cf. Missing data is not false.
inline bool Traverse(const ProbeReader& reader,std::uint32_t actor,Sample& sample,
                     CandleEnabledObservation& result) noexcept {
    if (!actor) return false;
    ProbeArray32 table{};
    if (!Read(reader,sample,kObjectTable,table.data) || !Read(reader,sample,kObjectTable+4,table.count) ||
        !Read(reader,sample,kObjectTable+8,table.capacity) || !ValidProbeArray(table,65536) ||
        !Member(reader,sample,table,actor)) return false;
    std::uint32_t actor_flags=0;
    if (!At(reader,sample,actor,0x1c,actor_flags) || (actor_flags&0x80000000u)) return false;
    std::uint32_t mask=0;if (!Descriptor(reader,sample,mask)) return false;
    std::array<std::uint32_t,kMaxObjects> objects{};
    std::size_t object_count=0, slots_seen=0, classes_seen=0;
    std::uint32_t current=actor;
    while (current) {
        if (object_count==objects.size()) return false;
        for (std::size_t i=0;i<object_count;++i) if (objects[i]==current) return false;
        objects[object_count++]=current;
        if (current!=actor && !Member(reader,sample,table,current)) return false;
        std::uint32_t hash=0;
        if (!At(reader,sample,current,0x28,hash)) return false;
        if (hash) {
            std::uint16_t index=0,previous_id=0;
            if (!At(reader,sample,hash,(kPropertyId&7)*2,index)) return false;
            std::array<std::uint16_t,kMaxSlots> seen{};
            std::size_t seen_count=0;
            while (index) {
                if (index>=kSlotCount || slots_seen==kMaxSlots || seen_count==seen.size()) return false;
                for (std::size_t i=0;i<seen_count;++i) if (seen[i]==index) return false;
                seen[seen_count++]=index;++slots_seen;
                std::uint64_t word=0;
                if (!At(reader,sample,kSlotPool,index*8,word)) return false;
                const auto next=static_cast<std::uint16_t>(word);
                const auto flags=static_cast<std::uint16_t>(word>>16);
                const auto value=static_cast<std::uint32_t>(word>>32);
                const auto id=static_cast<std::uint16_t>(flags&0x7fffu);
                if (seen_count>1 && id<=previous_id) return false;
                previous_id=id;
                if (id>kPropertyId) break;
                if (id==kPropertyId) {
                    std::uint32_t bits=value;
                    if (!(flags&0x8000u)) {
                        if (!value) break; // the native resolver continues into defaults
                        if (!Read(reader,sample,value,bits)) return false;
                    }
                    result.status=(bits&mask) ? CandleEnabledStatus::On : CandleEnabledStatus::Off;
                    result.inherited=current!=actor;
                    return true;
                }
                index=next;
            }
        }

        // Once the current object is already its class's default, native code
        // starts with the superclass. Otherwise it tries its own class first.
        std::uint32_t cls=0,default_object=0;
        if (!At(reader,sample,current,0x24,cls) || !Member(reader,sample,table,cls) ||
            !At(reader,sample,cls,0xe8,default_object)) return false;
        if (default_object==current && !At(reader,sample,cls,0x2c,cls)) return false;
        std::array<std::uint32_t,kMaxClasses> classes{};
        std::size_t class_count=0;
        current=0;
        while (cls) {
            if (class_count==classes.size() || classes_seen==kMaxClasses) return false;
            for (std::size_t i=0;i<class_count;++i) if (classes[i]==cls) return false;
            classes[class_count++]=cls;++classes_seen;
            if (!Member(reader,sample,table,cls) || !At(reader,sample,cls,0xe8,default_object)) return false;
            if (default_object) {current=default_object;break;}
            if (!At(reader,sample,cls,0x2c,cls)) return false;
        }
    }
    return false;
}

} // namespace candle_enabled_detail

inline CandleEnabledObservation SampleBedroomCandleEnabled(const ProbeReader& reader,std::uint32_t actor) noexcept {
    candle_enabled_detail::Sample sample;
    CandleEnabledObservation first,second;
    if (!candle_enabled_detail::Traverse(reader,actor,sample,first)) return {};
    sample.verifying=true;
    if (!candle_enabled_detail::Traverse(reader,actor,sample,second) || sample.cursor!=sample.count ||
        first.status!=second.status || first.inherited!=second.inherited) return {};
    return first;
}
} // namespace t3render::scene_bridge
