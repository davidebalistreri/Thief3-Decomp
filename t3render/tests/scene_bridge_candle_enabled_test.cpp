#include "../src/scene_bridge_candle_enabled.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>

using namespace t3render::scene_bridge;

struct Memory {
    std::map<std::uint32_t,std::uint8_t> bytes;
    std::uint32_t trigger=0,change=0,value=0;
    template<class T> void put(std::uint32_t address,T value) {
        std::uint8_t raw[sizeof(value)]{};std::memcpy(raw,&value,sizeof(value));
        for (std::size_t i=0;i<sizeof(value);++i) bytes[address+static_cast<std::uint32_t>(i)]=raw[i];
    }
    static bool read(void* context,std::uint32_t address,void* destination,std::size_t size) noexcept {
        auto& memory=*static_cast<Memory*>(context);
        for (std::size_t i=0;i<size;++i) if (!memory.bytes.count(address+static_cast<std::uint32_t>(i))) return false;
        for (std::size_t i=0;i<size;++i)
            static_cast<std::uint8_t*>(destination)[i]=memory.bytes[address+static_cast<std::uint32_t>(i)];
        if (address==memory.trigger) {memory.trigger=0;memory.put(memory.change,memory.value);}
        return true;
    }
};

constexpr std::uint32_t actor=0x1000,cls_a=0x3000,default_a=0x4000,cls_b=0x5000,
                        default_b=0x6000,cls_c=0x7000,default_c=0xe000;
constexpr std::uint32_t table_data=0xb000,descriptor=0xa000,hash_a=0xc000,hash_b=0xc100;
constexpr std::uint32_t pool=0x10f7b5c8;

void object(Memory& memory,std::uint32_t address,std::uint32_t index) {
    memory.put(address,0x10e40000u);memory.put(address+4,index);memory.put(address+0x18,0u);
    memory.put(address+0x20,100u+index);memory.put(table_data+index*4,address);
}
void property(Memory& memory,std::uint32_t hash,std::uint16_t slot,bool inlined,std::uint32_t value) {
    memory.put<std::uint16_t>(hash+14,slot);
    memory.put<std::uint16_t>(pool+slot*8,0);
    memory.put<std::uint16_t>(pool+slot*8+2,static_cast<std::uint16_t>((inlined ? 0x8000u : 0u)|0x5cfu));
    memory.put(pool+slot*8+4,value);
}
Memory Fixture() {
    Memory memory;
    memory.put(0x10f3e4a0u,table_data);memory.put(0x10f3e4a4u,7u);memory.put(0x10f3e4a8u,7u);
    object(memory,actor,0);object(memory,cls_a,1);object(memory,default_a,2);
    object(memory,cls_b,3);object(memory,default_b,4);object(memory,cls_c,5);object(memory,default_c,6);
    memory.put(actor+0x1c,0u);memory.put(actor+0x24,cls_a);memory.put(actor+0x28,hash_a);
    memory.put(default_a+0x1c,0u);memory.put(default_a+0x24,cls_a);memory.put(default_a+0x28,0u);
    memory.put(default_b+0x24,cls_b);memory.put(default_b+0x28,0u);
    memory.put(default_c+0x24,cls_c);memory.put(default_c+0x28,0u);
    memory.put(cls_a+0xe8,default_a);memory.put(cls_a+0x2c,cls_b);
    memory.put(cls_b+0xe8,default_b);memory.put(cls_b+0x2c,0u);
    memory.put(cls_c+0xe8,default_c);memory.put(cls_c+0x2c,0u);
    memory.put(0x10f3eda4u,0x8000u);memory.put(0x8008u,0x9000u);
    memory.put(0x9000u+0x5cfu*4,descriptor);memory.put(descriptor,0x10e4e7e0u);
    memory.put(0x10e4e7e0u+0xb4,0x10aecb20u);memory.put(0x10e4e7e0u+0xb0,0x10aed570u);
    memory.put(descriptor+0x34,1u);memory.put(descriptor+0x38,4u);memory.put(descriptor+0x60,4u);
    property(memory,hash_a,1,true,4u);
    return memory;
}
CandleEnabledObservation sample(Memory& memory) {return SampleBedroomCandleEnabled({&memory,Memory::read},actor);}

int main() {
    {
        auto memory=Fixture();auto value=sample(memory);
        assert(value.status==CandleEnabledStatus::On && !value.inherited);
        property(memory,hash_a,1,true,0xfffffffbu);value=sample(memory);
        assert(value.status==CandleEnabledStatus::Off && !value.inherited);
        property(memory,hash_a,1,false,0xd000u);memory.put(0xd000u,0u);value=sample(memory);
        assert(value.status==CandleEnabledStatus::Off && !value.inherited);
        memory.put(0xd000u,4u);assert(sample(memory).status==CandleEnabledStatus::On);
    }
    {
        auto memory=Fixture();memory.put(actor+0x28,0u);memory.put(default_a+0x28,hash_b);
        property(memory,hash_b,2,true,0u);auto value=sample(memory);
        assert(value.status==CandleEnabledStatus::Off && value.inherited);
        memory.put(default_a+0x1c,0x80000000u);property(memory,hash_b,2,true,4u);
        value=sample(memory);assert(value.status==CandleEnabledStatus::On && value.inherited);
        memory.put(actor+0x1c,0x80000000u);assert(sample(memory).status==CandleEnabledStatus::Unknown);
    }
    {
        auto memory=Fixture();property(memory,hash_a,1,false,0u);
        memory.put(default_a+0x28,hash_b);property(memory,hash_b,2,true,4u);
        const auto value=sample(memory);
        assert(value.status==CandleEnabledStatus::On && value.inherited);
    }
    {
        auto memory=Fixture();memory.put(actor+0x28,0u);
        memory.put(cls_b+0xe8,0u);memory.put(cls_b+0x2c,cls_c);
        memory.put(default_c+0x28,hash_b);property(memory,hash_b,2,true,4u);
        const auto value=sample(memory);
        assert(value.status==CandleEnabledStatus::On && value.inherited);
    }
    {
        auto memory=Fixture();memory.put(actor+0x28,0u);
        assert(sample(memory).status==CandleEnabledStatus::Unknown);
        memory.put(cls_b+0xe8,0u);memory.put(cls_b+0x2c,cls_b);
        assert(sample(memory).status==CandleEnabledStatus::Unknown);
    }
    for (const auto field : {descriptor,descriptor+0x34,descriptor+0x38,descriptor+0x60,
                             0x10e4e7e0u+0xb0,0x10e4e7e0u+0xb4}) {
        auto memory=Fixture();memory.put(field,0u);
        assert(sample(memory).status==CandleEnabledStatus::Unknown);
    }
    {
        auto memory=Fixture();memory.bytes.erase(actor+0x28);
        assert(sample(memory).status==CandleEnabledStatus::Unknown);
        memory=Fixture();memory.put(table_data+4,0u);
        memory.put(actor+0x28,0u);assert(sample(memory).status==CandleEnabledStatus::Unknown);
    }
    {
        auto memory=Fixture();memory.put<std::uint16_t>(pool+8,1);
        memory.put<std::uint16_t>(pool+10,0x5ce);
        assert(sample(memory).status==CandleEnabledStatus::Unknown);
    }
    {
        auto memory=Fixture();memory.trigger=pool+8;memory.change=pool+12;memory.value=0u;
        assert(sample(memory).status==CandleEnabledStatus::Unknown);
    }
}
