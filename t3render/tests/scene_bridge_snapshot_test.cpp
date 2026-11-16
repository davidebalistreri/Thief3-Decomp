#include "../src/scene_bridge_snapshot.hpp"
#include "../src/scene_bridge_submission.hpp"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>

using namespace t3render::scene_bridge;

struct Memory {
    std::map<std::uint32_t, std::uint8_t> bytes;
    unsigned owner_reads = 0;
    unsigned read_calls = 0;
    std::uint32_t table_base = 0, table_slots = 0;
    unsigned table_chunks = 0;
    void region(std::uint32_t address, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) bytes[address + static_cast<std::uint32_t>(i)] = 0;
    }
    template<class T> void put(std::uint32_t address, T value) {
        std::uint8_t buffer[sizeof(value)];
        std::memcpy(buffer, &value, sizeof(value));
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[address + static_cast<std::uint32_t>(i)] = buffer[i];
    }
    static bool read(void* context, std::uint32_t address, void* output, std::size_t count) noexcept {
        auto& memory = *static_cast<Memory*>(context);
        ++memory.read_calls;
        if (address >= 0x4000 && address < 0x4200) ++memory.owner_reads;
        if (memory.table_base && address >= memory.table_base &&
            (address-memory.table_base)%512==0) {
            const auto start=(address-memory.table_base)/4;
            if (start<memory.table_slots && count==std::min<std::uint32_t>(128,memory.table_slots-start)*4)
                ++memory.table_chunks;
        }
        auto* result = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0; i < count; ++i) {
            const auto found = memory.bytes.find(address + static_cast<std::uint32_t>(i));
            if (found == memory.bytes.end()) return false;
            result[i] = found->second;
        }
        return true;
    }
};

struct ChangeDuringWitness {
    enum class Kind { Outer, List, DuplicateObject };
    Memory* memory;
    Kind kind = Kind::Outer;
    bool changed = false;
    static bool read(void* context, std::uint32_t address, void* output, std::size_t count) noexcept {
        auto& state=*static_cast<ChangeDuringWitness*>(context);
        const bool ok=Memory::read(state.memory,address,output,count);
        if (ok && address==0x4018 && count==4 && !state.changed) {
            state.changed=true;
            if (state.kind==Kind::List) state.memory->put<std::uint32_t>(0x2000,0x7000);
            else if (state.kind==Kind::DuplicateObject) state.memory->put<std::uint32_t>(0x8000,0x4000);
            else state.memory->put<std::uint32_t>(0x4018,0x7000);
        }
        return ok;
    }
};

struct ChangeDuringNodeName {
    Memory* memory;
    bool changed=false;
    static bool read(void* context,std::uint32_t address,void* output,std::size_t count) noexcept {
        auto& state=*static_cast<ChangeDuringNodeName*>(context);
        const bool ok=Memory::read(state.memory,address,output,count);
        if (ok && address==0xf100 && count==32 && !state.changed) {
            state.changed=true;state.memory->put<std::uint8_t>(0xf101,'X');
        }
        return ok;
    }
};

struct ChangeDuringSharedName {
    Memory* memory;
    unsigned reads=0;
    static bool read(void* context,std::uint32_t address,void* output,std::size_t count) noexcept {
        auto& state=*static_cast<ChangeDuringSharedName*>(context);
        const bool ok=Memory::read(state.memory,address,output,count);
        if (ok && address==0x13004 && count==8 && ++state.reads==2)
            state.memory->put<std::uint8_t>(0x13005,'X');
        return ok;
    }
};

struct ChangeDuringOwnerFields {
    Memory* memory;
    std::uint32_t changed_address;
    bool changed=false;
    static bool read(void* context,std::uint32_t address,void* output,std::size_t count) noexcept {
        auto& state=*static_cast<ChangeDuringOwnerFields*>(context);
        const bool ok=Memory::read(state.memory,address,output,count);
        if (ok && address==0x4020 && count==36 && !state.changed) {
            state.changed=true;state.memory->put<std::uint32_t>(state.changed_address,0x12345678);
        }
        return ok;
    }
};

struct ChangeDuringNamedPair {
    Memory* memory;
    bool changed=false;
    static bool read(void* context,std::uint32_t address,void* output,std::size_t count) noexcept {
        auto& state=*static_cast<ChangeDuringNamedPair*>(context);
        const bool ok=Memory::read(state.memory,address,output,count);
        if (ok && address==0x8000 && count==20 && !state.changed) {
            state.changed=true;state.memory->put<std::uint32_t>(0x8004,0);
        }
        return ok;
    }
};

struct LimitedRead {
    Memory* memory;
    static bool read(void* context,std::uint32_t address,void* output,std::size_t count) noexcept {
        auto& state=*static_cast<LimitedRead*>(context);
        return count<=12 && Memory::read(state.memory,address,output,count);
    }
};

void bounded_owner_groups() {
    Memory sparse;sparse.region(0x20020,8);sparse.region(0x2002c,24);
    sparse.put<std::uint32_t>(0x20020,0x1234);sparse.put<std::uint32_t>(0x20024,0x5678);
    for (unsigned i=0;i<6;++i) sparse.put<std::uint32_t>(0x2002c+i*4,10+i);
    ProbeReader reader{&sparse,Memory::read};ProbeEntry entry;
    ReadOwnerNamedPose(reader,0x20000,entry);
    assert(entry.owner_fname.available && entry.owner_fname.value==0x1234 &&
           entry.owner_class.available && entry.owner_class.value==0x5678 &&
           entry.owner_location_available && entry.owner_rotation_available &&
           entry.owner_location_bits[2]==12 && entry.owner_rotation_bits[2]==15);
    assert(sparse.read_calls==5); // bulk fails on the inaccessible +0x28 bytes
    LimitedRead limited{&sparse};ProbeReader fragmented{&limited,LimitedRead::read};
    std::uint32_t location[3]{},rotation[3]{};
    assert(ReadOwnerPose(fragmented,0x20000,location,rotation) && location[2]==12 && rotation[2]==15);
}

void bounded_mesh_names() {
    for (std::size_t length:{std::size_t(1),std::size_t(31),std::size_t(32),std::size_t(33),
                             std::size_t(63),std::size_t(64),std::size_t(95),std::size_t(96),std::size_t(127)}) {
        Memory memory;memory.region(0x20000,128);
        for (std::size_t i=0;i<length;++i) memory.put<std::uint8_t>(0x20000+static_cast<std::uint32_t>(i),'A');
        ProbeReader reader{&memory,Memory::read};char name[128]{};
        assert(ReadMeshNodeName(reader,0x20000,name) && std::strlen(name)==length);
        assert(memory.read_calls==length/32+1);
    }
    Memory sparse;sparse.region(0x30000,5);
    for (unsigned i=0;i<4;++i) sparse.put<std::uint8_t>(0x30000+i,'A'+i);
    ProbeReader reader{&sparse,Memory::read};char name[128]{};
    assert(ReadMeshNodeName(reader,0x30000,name) && std::strcmp(name,"ABCD")==0 && sparse.read_calls==6);
    sparse.put<std::uint8_t>(0x30002,0x80);assert(!ReadMeshNodeName(reader,0x30000,name));
    Memory long_name;long_name.region(0x40000,128);
    for (unsigned i=0;i<128;++i) long_name.put<std::uint8_t>(0x40000+i,'A');
    reader.context=&long_name;assert(!ReadMeshNodeName(reader,0x40000,name));
    Memory boundary;boundary.region(0xfffffffcu,4);
    boundary.put<std::uint8_t>(0xfffffffcu,'A');boundary.put<std::uint8_t>(0xfffffffdu,'B');
    boundary.put<std::uint8_t>(0xfffffffeu,'C');reader.context=&boundary;
    assert(ReadMeshNodeName(reader,0xfffffffcu,name) && std::strcmp(name,"ABC")==0);
}

Memory SampleMemory(bool owner_member = true) {
    Memory m;
    m.region(0x1000, 0x100); m.region(0x2000, 512); m.region(0x3000, 0x100);
    m.region(0x4000, 0x200); m.region(0x8000, 64); m.region(0xa000, 64); m.region(0xb000, 128);
    m.region(0x10f3e4a0, 12); m.region(0x10f7af1c, 12); m.region(0x10efef54, 4);
    m.put<std::uint32_t>(0x10efef54, 1);
    m.put<std::uint32_t>(0x1050, 1); m.put<std::uint32_t>(0x1058, 0x2000);
    m.put<std::uint32_t>(0x2000, 0x3000); m.put<std::uint32_t>(0x3000, 0x10e4e8c0);
    m.put<std::uint32_t>(0x3044, 4); m.put<std::uint8_t>(0x3048, 1); m.put<std::uint32_t>(0x3054, 0x4000);
    m.put<std::uint32_t>(0x10f3e4a0, 0x8000); m.put<std::uint32_t>(0x10f3e4a4, 3);
    m.put<std::uint32_t>(0x10f3e4a8, 3); m.put<std::uint32_t>(0x8008, owner_member ? 0x4000 : 0);
    m.put<std::uint32_t>(0x4004, 2); m.put<std::uint32_t>(0x4020, (1521u << 16) | 7);
    m.put<std::uint32_t>(0x4024, 0x7000); m.put<std::uint32_t>(0x40b4, 0x3000);
    m.put<std::uint32_t>(0x10f7af1c, 0xa000); m.put<std::uint32_t>(0x10f7af20, 8);
    m.put<std::uint32_t>(0x10f7af24, 8); m.put<std::uint32_t>(0xa01c, 0xb000);
    const char name[] = "StaticMeshActor";
    for (std::size_t i = 0; i < sizeof(name); ++i) m.put<std::uint8_t>(0xb01c + static_cast<std::uint32_t>(i), name[i]);
    return m;
}

int main() {
    bounded_mesh_names();
    bounded_owner_groups();
    auto memory = SampleMemory();
    ProbeReader reader{&memory, Memory::read};
    auto sample = SampleProbe(reader, 42, 0x1000, 0x12345678);
    assert(sample.thread_id == 42 && sample.self == 0x1000 && sample.caller == 0x12345678);
    assert(sample.pass.available && sample.pass.value == 1);
    assert(sample.count.available && sample.count.value == 1 && sample.entries_attempted == 1);
    assert(sample.object_table_complete);
    const auto& entry = sample.entries[0];
    assert(entry.owner_table_member && entry.owner_stable && entry.owner_name_available);
    assert(std::strcmp(entry.owner_name, "StaticMeshActor") == 0);
    assert(entry.owner_fname.value == ((1521u << 16) | 7) && entry.owner_backlink.value == 0x3000);

    auto absent = SampleMemory(false);
    reader.context = &absent;
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.object_table_complete && !sample.entries[0].owner_table_member && absent.owner_reads == 0);

    auto duplicate = SampleMemory();
    duplicate.put<std::uint32_t>(0x8004, 0x4000);
    reader.context = &duplicate;
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.object_table_complete && !sample.entries[0].owner_table_member);

    auto malformed = SampleMemory();
    malformed.put<std::uint32_t>(0x1050, 200);
    reader.context = &malformed;
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.entries_attempted == kMaxProbeEntries && sample.entries_truncated);

    auto beyond = SampleMemory();
    beyond.region(0x2000, 200 * 4);
    beyond.put<std::uint32_t>(0x1050, 200);
    beyond.put<std::uint32_t>(0x2000 + 150 * 4, 0x3000);
    reader.context = &beyond;
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.entries_truncated && sample.target.object_scan_complete && sample.target.list_scan_complete);
    assert(sample.target.actors_named_target == 1 && sample.target.list_occurrences == 1);
    assert(sample.target.entry_available && sample.target.entry.list_index == 150 && sample.target.entry.owner_stable);

    beyond.put<std::uint32_t>(0x2000 + 151 * 4, 0x3000);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.list_scan_complete && sample.target.list_occurrences == 2 && !sample.target.entry_available);

    beyond.put<std::uint32_t>(0x8004, 0x4000);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.actors_named_target == 2 && !sample.target.entry_available);

    // A fully synthetic UObject class/property/resource chain: identification
    // remains a candidate even when all independent guards agree.
    auto complete = SampleMemory();
    complete.region(0x7000, 128); complete.region(0x9000, 32);
    complete.region(0xc000, 128); complete.region(0xd000, 32); complete.region(0xe000, 32);
    complete.region(0x6000, 16); complete.region(0x6100, 0x100); complete.region(0x6200, 0x40);
    complete.region(0x3100, 16); complete.region(0xf000, 0xc0); complete.region(0xf100, 32);
    complete.region(0x10f31bb8, 12);
    complete.region(0x11000, 40); complete.region(0x12000, 3 * 36);
    complete.region(0x13000, 32); complete.region(0x14000, 32);
    complete.region(0x14100, 32); complete.region(0x14200, 32);
    complete.region(0x10f2c740, 4); complete.region(0x15000, 32); complete.region(0x16000, 0x58);
    complete.region(0x17000, 4 + 2 * 0x1ac); complete.region(0x18000, 4 + 2 * 36);
    complete.region(0x19000, 32); complete.region(0x19100, 32);
    complete.region(0x1a000, 64); complete.region(0x1b000, 128);
    complete.region(0x10f3eda4, 4); complete.region(0x10e72e48 + 0xb4, 4);
    complete.region(0x10f7b5c8 + 3 * 8, 8);
    complete.put<std::uint32_t>(0x10f3e4a4, 5); complete.put<std::uint32_t>(0x10f3e4a8, 5);
    complete.put<std::uint32_t>(0x8004, 0x7000); complete.put<std::uint32_t>(0x800c, 0xc000);
    complete.put<std::uint32_t>(0x8010, 0x1a000); complete.put<std::uint32_t>(0x4018, 0x1a000);
    complete.put<std::uint32_t>(0x1a004, 4); complete.put<std::uint32_t>(0x1a020, 8);
    complete.put<std::uint32_t>(0x10f7af20, 9); complete.put<std::uint32_t>(0x10f7af24, 9);
    complete.put<std::uint32_t>(0xa020, 0x1b000);
    const char map_name[] = "LOADGAME:bedroom";
    for (std::size_t i=0;i<sizeof(map_name);++i) complete.put<std::uint8_t>(0x1b01c+i,map_name[i]);
    complete.put<std::uint32_t>(0x7004, 1); complete.put<std::uint32_t>(0x7020, 7);
    complete.put<std::uint32_t>(0xc004, 3);
    complete.put<std::uint32_t>(0x10f3eda4, 0x6000); complete.put<std::uint32_t>(0x6008, 0x6100);
    complete.put<std::uint32_t>(0x6100 + 0x2b * 4, 0x6200); complete.put<std::uint32_t>(0x6200, 0x10e72e48);
    complete.put<std::uint32_t>(0x10e72e48 + 0xb4, 0x10aeecc0);
    complete.put<std::uint32_t>(0x6234, 1); complete.put<std::uint32_t>(0x6238, 4);
    complete.put<std::uint32_t>(0x4028, 0x9000); complete.put<std::uint16_t>(0x9006, 3);
    const auto property = 0x10f7b5c8u + 3 * 8;
    complete.put<std::uint16_t>(property + 2, 0x802b); complete.put<std::uint32_t>(property + 4, 0xc000);
    complete.put<std::uint32_t>(0xc000, 0x10e4dfa8); complete.put<std::uint32_t>(0xc058, 0xd000);
    complete.put<std::uint32_t>(0xd000, 0x10ec0e20); complete.put<std::uint32_t>(0xd008, 0xe004);
    const char mesh_name[] = "SyntheticMesh";
    complete.put<std::uint32_t>(0xe000, sizeof(mesh_name) - 1);
    for (std::size_t i = 0; i < sizeof(mesh_name); ++i)
        complete.put<std::uint8_t>(0xe004 + static_cast<std::uint32_t>(i), mesh_name[i]);
    complete.put<std::uint32_t>(0x3108, 0xf000);
    complete.put<std::uint32_t>(0x10f31bb8, 0xf000); complete.put<std::uint32_t>(0x10f31bbc, 0xf000);
    complete.put<std::uint32_t>(0x10f31bc0, 1); complete.put<std::uint32_t>(0xf000, 0x10e49d14);
    complete.put<std::uint32_t>(0xf004, 257); complete.put<std::uint32_t>(0xf008, 236);
    complete.put<std::uint32_t>(0xf00c, 708); complete.put<std::uint32_t>(0xf010, 3);
    for (std::uint32_t i = 0; i < 6; ++i) complete.put<float>(0xf074 + i * 4, static_cast<float>(i));
    complete.put<std::uint32_t>(0xf0b0, 0xf100);
    complete.put<std::uint32_t>(0x3104, 0x11004); complete.put<std::uint32_t>(0x11000, 3);
    complete.put<std::uint32_t>(0xf030, 0x12000);
    const auto put_string = [&](std::uint32_t address, const char* text) {
        const auto length = static_cast<std::uint32_t>(std::strlen(text));
        complete.put<std::uint32_t>(address - 4, length);
        for (std::uint32_t i = 0; i <= length; ++i) complete.put<std::uint8_t>(address + i, text[i]);
    };
    put_string(0x13004, "Default"); put_string(0x14004, "BF");
    put_string(0x14104, "GENgoodnessA_D"); put_string(0x14204, "GENiFABRICwoolA2_D");
    const std::uint32_t starts[] = {0, 24, 504}, sizes[] = {24, 480, 204};
    const std::uint32_t names[] = {0x14004, 0x14104, 0x14204};
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto section = 0x12000u + i * 36;
        complete.put<std::uint32_t>(section, 0x13004);
        complete.put<std::uint32_t>(section + 12, sizes[i] / 3);
        complete.put<std::uint32_t>(section + 20, 257);
        complete.put<std::uint32_t>(section + 24, starts[i]);
        complete.put<std::uint32_t>(section + 28, sizes[i]);
        complete.put<std::uint32_t>(0x11004 + i * 12, names[i]);
    }
    constexpr std::uint32_t bsp_block = 0x17004 + 0x1ac;
    complete.put<std::uint32_t>(0x1050, 2); complete.put<std::uint32_t>(0x2004, bsp_block);
    complete.put<std::uint32_t>(0x10f2c740, 0x15000); complete.put<std::uint32_t>(0x1500c, 0x16000);
    complete.put<std::uint32_t>(0x16000, 0x50534256); complete.put<std::uint32_t>(0x1600c, 2);
    complete.put<std::uint32_t>(0x16054, 0x17004); complete.put<std::uint32_t>(0x17000, 2);
    complete.put<std::uint32_t>(bsp_block, 0x10e4a668); complete.put<std::uint32_t>(bsp_block + 0x44, 5);
    complete.put<std::uint32_t>(bsp_block + 0xe4, 60); complete.put<std::uint32_t>(bsp_block + 0xe8, 38);
    complete.put<std::uint32_t>(bsp_block + 0xec, 114); complete.put<std::uint32_t>(bsp_block + 0xf0, 2);
    for (std::uint32_t i = 0; i < 16; ++i) complete.put<float>(bsp_block + 4 + i * 4, i % 5 == 0 ? 1.f : 0.f);
    for (std::uint32_t i = 0; i < 6; ++i) complete.put<float>(bsp_block + 0x17c + i * 4, static_cast<float>(i));
    complete.put<std::uint32_t>(bsp_block + 0x130, 0x18004); complete.put<std::uint32_t>(0x18000, 2);
    put_string(0x19004, "wall"); put_string(0x19104, "floor");
    for (std::uint32_t i = 0; i < 2; ++i) {
        const auto section = 0x18004u + i * 36;
        complete.put<std::uint32_t>(section, i ? 0x19104 : 0x19004);
        complete.put<std::uint32_t>(section + 12, 19); complete.put<std::uint32_t>(section + 20, 60);
        complete.put<std::uint32_t>(section + 24, i * 57); complete.put<std::uint32_t>(section + 28, 57);
    }
    for (std::size_t i = 0; i < sizeof(mesh_name); ++i)
        complete.put<std::uint8_t>(0xf100 + static_cast<std::uint32_t>(i), mesh_name[i]);
    reader.context = &complete;
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.object_scan_complete && sample.target.list_scan_complete && sample.target.entry_available);
    assert(sample.target.owner_class.stable && sample.target.owner_class.name_available);
    assert(sample.target.asset.status == AssetStatus::Candidate && sample.target.asset.stable);
    assert(std::strcmp(sample.target.asset.name, mesh_name) == 0);
    assert(sample.target.mesh_node.pool_scan_complete && sample.target.mesh_node.nodes_scanned == 1 &&
           sample.target.mesh_node.occurrences == 1 && sample.target.mesh_node.stable);
    assert(sample.target.mesh_node.vertex_count == 257 && sample.target.mesh_node.index_count == 708 &&
           sample.target.mesh_node.triangle_count == 236 && sample.target.mesh_node.section_count == 3 &&
           std::strcmp(sample.target.mesh_node.name, mesh_name) == 0);
    assert(sample.target.mesh_node.sections_available && sample.target.mesh_node.sections_copied == 3 &&
           std::strcmp(sample.target.mesh_node.sections[1].name, "Default") == 0 &&
           std::strcmp(sample.target.mesh_node.sections[1].material_name, "GENgoodnessA_D") == 0 &&
           sample.target.mesh_node.sections[1].first_index == 24 &&
           sample.target.mesh_node.sections[1].index_count == 480);
    ProbeArray32 pair_table{0x8000,5,5};ProbeNamedObject pair_class,pair_map;
    assert(ReadNamedProbePair(reader,pair_table,{0x7000,true},{0x1a000,true},pair_class,pair_map) &&
           pair_class.index.value==1 && pair_map.index.value==4 && pair_class.stable && pair_map.stable);
    auto duplicate_pair=complete;duplicate_pair.put<std::uint32_t>(0x8000,0x7000);
    ProbeReader duplicate_pair_reader{&duplicate_pair,Memory::read};
    ProbeNamedObject untouched_class,untouched_map;untouched_class.address={0x55,true};untouched_map.address={0x66,true};
    assert(!ReadNamedProbePair(duplicate_pair_reader,pair_table,{0x7000,true},{0x1a000,true},untouched_class,untouched_map));
    assert(untouched_class.address.value==0x55 && untouched_map.address.value==0x66);
    assert(!ReadNamedProbePair(reader,pair_table,{0x7000,true},{0x7000,true},untouched_class,untouched_map));
    auto changing_pair=complete;ChangeDuringNamedPair pair_state{&changing_pair,false};
    ProbeReader changing_pair_reader{&pair_state,ChangeDuringNamedPair::read};
    assert(!ReadNamedProbePair(changing_pair_reader,pair_table,{0x7000,true},{0x1a000,true},untouched_class,untouched_map));
    assert(pair_state.changed && untouched_class.address.value==0x55 && untouched_map.address.value==0x66);
    auto changed_name=complete;ChangeDuringNodeName changed_name_state{&changed_name,false};
    ProbeReader changed_name_reader{&changed_name_state,ChangeDuringNodeName::read};ProbeMeshNode changing_node;
    ReadProbeMeshNode(changed_name_reader,0x3000,changing_node);
    assert(changed_name_state.changed && !changing_node.stable);
    auto shared_names=complete;shared_names.read_calls=0;
    ProbeReader shared_reader{&shared_names,Memory::read};ProbeMeshNode shared_node;
    ReadProbeMeshNode(shared_reader,0x3000,shared_node);const auto shared_reads=shared_names.read_calls;
    assert(shared_node.stable && shared_node.sections_available);
    auto unique_names=complete;unique_names.region(0x1c000,64);
    for (std::uint32_t i=1;i<3;++i) {
        const auto at=0x1c004u+(i-1)*32;const char text[]="Default";
        unique_names.put<std::uint32_t>(at-4,7);
        for (std::uint32_t j=0;j<sizeof(text);++j) unique_names.put<std::uint8_t>(at+j,text[j]);
        unique_names.put<std::uint32_t>(0x12000+i*36,at);
    }
    unique_names.read_calls=0;ProbeReader unique_reader{&unique_names,Memory::read};ProbeMeshNode unique_node;
    ReadProbeMeshNode(unique_reader,0x3000,unique_node);
    assert(unique_node.stable && unique_node.sections_available && shared_reads+4<=unique_names.read_calls);
    for (unsigned i=0;i<3;++i) assert(std::strcmp(shared_node.sections[i].name,unique_node.sections[i].name)==0);
    auto changed_shared=complete;ChangeDuringSharedName changed_shared_state{&changed_shared,0};
    ProbeReader changed_shared_reader{&changed_shared_state,ChangeDuringSharedName::read};ProbeMeshNode unstable_sections;
    ReadProbeMeshNode(changed_shared_reader,0x3000,unstable_sections);
    assert(changed_shared_state.reads>=2 && !unstable_sections.sections_available);
    assert(sample.bsp.source_stable && sample.bsp.block_count == 2 && sample.bsp.type5_seen == 1 &&
           sample.bsp.member_count == 1 && sample.bsp.candidates_copied == 1);
    const auto& bsp = sample.bsp.candidates[0];
    assert(bsp.stable && bsp.source_index == 1 && bsp.list_index == 1 &&
           bsp.vertex_count == 60 && bsp.index_count == 114 && bsp.sections_available &&
           bsp.sections_copied == 2 && std::strcmp(bsp.sections[0].material_name, "wall") == 0);
    // A different visible actor can attest its own outer even when the target
    // bed is absent. It does not assert ownership of the VBSP.
    auto no_bed=complete;no_bed.put<std::uint32_t>(0x4020,(2u<<16)|7);no_bed.put<std::uint32_t>(0x10efef54,0);
    reader.context=&no_bed;
    auto witness=SampleLiveProbe(reader,42,0x1000,0x109271ae);
    assert(!witness.target.entry_available && witness.map_witness.available && witness.map_witness.list_stable &&
           witness.map_witness.object_table_complete && witness.map_witness.entry.owner_stable &&
           witness.map_witness.entry.list_index==0 && std::strcmp(witness.map_witness.outer.name,map_name)==0 &&
           witness.bsp.source_stable);
    witness=SampleProbe(reader,42,0x1000,0x109271ae);
    assert(!witness.target.entry_available && witness.map_witness.available);
    no_bed.put<std::uint32_t>(0x1a020,(1u<<16)|8);
    witness=SampleLiveProbe(reader,42,0x1000,0x109271ae);assert(!witness.map_witness.available);
    no_bed.put<std::uint32_t>(0x1a020,8);no_bed.put<std::uint32_t>(0x1a018,0x7000);
    witness=SampleLiveProbe(reader,42,0x1000,0x109271ae);assert(!witness.map_witness.available);
    no_bed.put<std::uint32_t>(0x1a018,0);no_bed.put<std::uint32_t>(0x40b4,0);
    witness=SampleLiveProbe(reader,42,0x1000,0x109271ae);assert(!witness.map_witness.available);
    no_bed.put<std::uint32_t>(0x40b4,0x3000);no_bed.put<std::uint32_t>(0x1050,3);no_bed.put<std::uint32_t>(0x2008,0x3000);
    witness=SampleLiveProbe(reader,42,0x1000,0x109271ae);assert(!witness.map_witness.available);
    for (auto kind:{ChangeDuringWitness::Kind::Outer,ChangeDuringWitness::Kind::List,ChangeDuringWitness::Kind::DuplicateObject})
        for (bool full:{false,true}) {
            auto changing=complete;changing.put<std::uint32_t>(0x4020,(2u<<16)|7);changing.put<std::uint32_t>(0x10efef54,0);
            ChangeDuringWitness state{&changing,kind,false};ProbeReader unstable{&state,ChangeDuringWitness::read};
            witness=full ? SampleProbe(unstable,42,0x1000,0x109271ae) : SampleLiveProbe(unstable,42,0x1000,0x109271ae);
            assert(state.changed && !witness.map_witness.available);
        }
    reader.context=&complete;
    // The live path checks only the visible actor's identity; it must not scan
    // each unrelated UObject by name on every game frame.
    auto large=complete;
    constexpr std::uint32_t extra=1000, table_base=0x200000, object_base=0x300000;
    large.region(table_base,(extra+5)*4);
    for (std::uint32_t i=0;i<5;++i) {
        std::uint32_t value=0;
        assert(Memory::read(&large,0x8000+i*4,&value,sizeof(value)));
        large.put<std::uint32_t>(table_base+i*4,value);
    }
    for (std::uint32_t i=0;i<extra;++i) {
        const auto object=object_base+i*64;large.region(object,0x24);
        large.put<std::uint32_t>(table_base+(i+5)*4,object);
    }
    large.put<std::uint32_t>(0x10f3e4a0,table_base);
    large.put<std::uint32_t>(0x10f3e4a4,extra+5);large.put<std::uint32_t>(0x10f3e4a8,extra+5);
    large.table_base=table_base;large.table_slots=extra+5;
    large.put<std::uint32_t>(0x10efef54,0);
    reader.context=&large;large.read_calls=0;
    auto full=SampleProbe(reader,42,0x1000,0x109271ae);const auto full_reads=large.read_calls;
    large.read_calls=0;auto live=SampleLiveProbe(reader,42,0x1000,0x109271ae);const auto live_reads=large.read_calls;
    assert(full.target.object_scan_complete && full.target.entry_available);
    assert(live.target.visible_list_only && !live.target.object_scan_complete && live.target.list_scan_complete &&
           live.target.list_occurrences==1 && live.target.entry_available && live.target.owner_class.stable &&
           live.target.asset.status==AssetStatus::Candidate && live.target.mesh_node.stable && live.bsp.source_stable);
    assert(live_reads*2<full_reads);
    large.put<std::uint32_t>(0x4020,(2u<<16)|7);large.read_calls=0;
    auto witness_live=SampleLiveProbe(reader,42,0x1000,0x109271ae);const auto witness_reads=large.read_calls;
    assert(!witness_live.target.entry_available && witness_live.map_witness.available && witness_reads<full_reads);
    std::printf("probe_read_calls full=%u live=%u no_bed=%u unrelated_objects=%u\n",full_reads,live_reads,witness_reads,extra);
    large.put<std::uint32_t>(0x10f2c734,0x1000);large.read_calls=0;large.table_chunks=0;
    auto first_generic=SampleSubmission(reader,0x3000);
    const auto generic_observation=SampleVisibleStaticMesh(reader,witness_live,first_generic);
    assert(AttachAssetCandidate(first_generic,generic_observation.asset));
    auto last_generic=SampleSubmission(reader,0x3000);
    const AssetReader generic_reader{&large,Memory::read};
    assert(MatchesAssetCandidate(generic_reader,last_generic.owner,generic_observation.asset));
    assert(AttachAssetCandidate(last_generic,generic_observation.asset));
    const auto generic_reads=large.read_calls;
    assert(generic_observation.list_stable && generic_observation.asset.status==AssetStatus::Candidate &&
           generic_observation.mesh_node.stable && SameSubmission(first_generic,last_generic));
    assert(large.table_chunks==5*((large.table_slots+127)/128));
    std::printf("generic_scope_read_calls=%u table_chunks=%u unrelated_objects=%u\n",
                generic_reads,large.table_chunks,extra);
    // Expand the table without allocating unrelated objects. The generic path
    // may read table slots, but must not dereference those other entries.
    auto huge=complete;
    constexpr std::uint32_t huge_extra=16000, huge_slots=huge_extra+5;
    huge.region(table_base,huge_slots*4);
    for (std::uint32_t i=0;i<5;++i) {
        std::uint32_t value=0;
        assert(Memory::read(&huge,0x8000+i*4,&value,sizeof(value)));
        huge.put<std::uint32_t>(table_base+i*4,value);
    }
    for (std::uint32_t i=0;i<huge_extra;++i)
        huge.put<std::uint32_t>(table_base+(i+5)*4,object_base+i*64);
    huge.put<std::uint32_t>(0x10f3e4a0,table_base);
    huge.put<std::uint32_t>(0x10f3e4a4,huge_slots);huge.put<std::uint32_t>(0x10f3e4a8,huge_slots);
    huge.put<std::uint32_t>(0x10efef54,0);huge.put<std::uint32_t>(0x10f2c734,0x1000);
    huge.put<std::uint32_t>(0x4020,(2u<<16)|7);
    huge.table_base=table_base;huge.table_slots=huge_slots;
    ProbeReader huge_reader{&huge,Memory::read};
    const auto huge_probe=SampleLiveProbe(huge_reader,42,0x1000,0x109271ae);
    assert(huge_probe.map_witness.available);
    huge.table_chunks=0;huge.read_calls=0;
    const auto huge_first=SampleSubmission(huge_reader,0x3000);
    const auto huge_observation=SampleVisibleStaticMesh(huge_reader,huge_probe,huge_first);
    assert(huge_observation.list_stable && huge_observation.asset.status==AssetStatus::Candidate &&
           huge_observation.mesh_node.stable);
    const auto expected_chunks=5*((huge_slots+127)/128);
    assert(huge.table_chunks==expected_chunks);
    std::printf("generic_large_table slots=%u chunks=%u reads=%u\n",
                huge_slots,huge.table_chunks,huge.read_calls);
    std::printf("probe_snapshot_bytes=%zu generic_sample_bytes=%zu\n",sizeof(ProbeSnapshot),sizeof(ProbeStaticMesh));
    large.put<std::uint32_t>(0x4020,(1521u<<16)|7);
    // Another globally named actor outside the visible list is not ruled out
    // by the fast path; its result must explicitly retain the weaker scope.
    large.put<std::uint32_t>(object_base+0x20,(1521u<<16)|7);
    full=SampleProbe(reader,42,0x1000,0x109271ae);
    live=SampleLiveProbe(reader,42,0x1000,0x109271ae);
    assert(full.target.actors_named_target==2 && !full.target.entry_available && !full.map_witness.available);
    assert(live.target.visible_list_only && live.target.list_occurrences==1 && live.target.entry_available);
    large.put<std::uint32_t>(object_base+0x20,0);
    large.put<std::uint32_t>(0x1050,3);large.put<std::uint32_t>(0x2008,0x3000);
    live=SampleLiveProbe(reader,42,0x1000,0x109271ae);
    assert(live.target.list_occurrences==2 && !live.target.entry_available);
    large.put<std::uint32_t>(0x10efef54,1);large.read_calls=0;
    live=SampleLiveProbe(reader,42,0x1000,0x109271ae);
    assert(live.pass_stable && live.pass.value==1 && large.read_calls==2 && !live.target.entry_available);
    reader.context=&complete;
    complete.put<std::uint32_t>(0x10f2c734, 0x1000);
    auto general=complete;general.put<std::uint32_t>(0x4020,(2u<<16)|7);
    general.put<std::uint32_t>(0x10efef54,0);reader.context=&general;
    const auto generic_probe=SampleLiveProbe(reader,42,0x1000,0x109271ae);
    const auto cheap=SampleSubmission(reader,0x3000);
    assert(cheap.valid && !cheap.asset_available);
    const auto generic_submission=SampleSubmission(reader,0x3000,true);
    assert(generic_submission.valid && generic_submission.asset_available &&
           std::strcmp(generic_submission.asset_name,mesh_name)==0);
    auto generic=SampleVisibleStaticMesh(reader,generic_probe,generic_submission);
    assert(generic.list_stable && generic.object_table_complete && generic.name_occurrences==1 &&
           generic.entry.owner_stable && generic.entry.list_index==0 && generic.owner_class.stable &&
           generic.outer.stable && generic.owner_outer.value==0x1a000 &&
           generic.asset.status==AssetStatus::Candidate && generic.mesh_node.stable && generic.mesh_node.sections_available);
    for (const auto address:{0x4020u,0x4024u,0x402cu,0x4038u}) {
        auto changing=general;ChangeDuringOwnerFields state{&changing,address,false};
        ProbeReader unstable{&state,ChangeDuringOwnerFields::read};
        assert(!SampleVisibleStaticMesh(unstable,generic_probe,generic_submission).list_stable && state.changed);
    }
    const AssetReader audit_reader{&general,Memory::read};
    assert(MatchesAssetCandidate(audit_reader,0x4000,generic.asset));
    auto bad_audit=generic.asset;bad_audit.audit.slot_value++;
    assert(!MatchesAssetCandidate(audit_reader,0x4000,bad_audit));
    for (const auto address:{0xc058u,0x6238u,0x800cu,0xd008u}) {
        auto changed=general;changed.put<std::uint32_t>(address,0);
        const AssetReader changed_reader{&changed,Memory::read};
        assert(!MatchesAssetCandidate(changed_reader,0x4000,generic.asset));
    }
    auto duplicated_probe=generic_probe;duplicated_probe.entries[2]=duplicated_probe.entries[0];
    duplicated_probe.entries[2].list_index=2;duplicated_probe.count.value=duplicated_probe.entries_attempted=3;
    assert(!SampleVisibleStaticMesh(reader,duplicated_probe,generic_submission).list_stable);
    auto duplicate_name=general;duplicate_name.region(0x3300,0x100);duplicate_name.region(0x4200,0x100);
    duplicate_name.put<std::uint32_t>(0x1050,3);duplicate_name.put<std::uint32_t>(0x2008,0x3300);
    duplicate_name.put<std::uint32_t>(0x3300,0x10e4e8c0);duplicate_name.put<std::uint32_t>(0x3344,4);
    duplicate_name.put<std::uint32_t>(0x3354,0x4200);duplicate_name.put<std::uint32_t>(0x4220,(2u<<16)|7);
    auto duplicate_list=generic_probe;duplicate_list.count.value=duplicate_list.entries_attempted=3;
    duplicate_list.entries[2].list_index=2;duplicate_list.entries[2].address={0x3300,true};
    duplicate_list.entries[2].vtable={0x10e4e8c0,true};duplicate_list.entries[2].type={4,true};
    ProbeReader duplicate_reader{&duplicate_name,Memory::read};
    assert(!SampleVisibleStaticMesh(duplicate_reader,duplicate_list,generic_submission).list_stable);
    for (auto kind:{ChangeDuringWitness::Kind::Outer,ChangeDuringWitness::Kind::List,ChangeDuringWitness::Kind::DuplicateObject}) {
        auto changing=general;ChangeDuringWitness state{&changing,kind,false};ProbeReader unstable{&state,ChangeDuringWitness::read};
        assert(!SampleVisibleStaticMesh(unstable,generic_probe,generic_submission).list_stable && state.changed);
    }
    reader.context=&complete;
    auto mesh_submission = SampleSubmission(reader, 0x3000);
    assert(mesh_submission.valid && mesh_submission.owner == 0x4000 && mesh_submission.mesh_node == 0xf000 &&
           mesh_submission.asset_available && mesh_submission.asset_proxy == 0xc000 &&
           mesh_submission.asset_resource == 0xd000 && std::strcmp(mesh_submission.asset_name, mesh_name) == 0);
    auto bsp_submission = SampleSubmission(reader, bsp_block);
    assert(bsp_submission.valid && bsp_submission.bsp_index == 1 && bsp_submission.bsp_source == 0x16000);
    assert(SameSubmission(mesh_submission, SampleSubmission(reader, 0x3000)));
    complete.put<std::uint32_t>(0x40b4, 0);
    assert(!SampleSubmission(reader, 0x3000).valid);
    complete.put<std::uint32_t>(0x40b4, 0x3000);
    complete.put<std::uint32_t>(0xd008, 0);
    assert(!SameSubmission(mesh_submission, SampleSubmission(reader, 0x3000)));
    complete.put<std::uint32_t>(0xd008, 0xe004);
    complete.put<std::uint32_t>(0x17000, 1);
    assert(!SampleSubmission(reader, bsp_block).valid);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(!sample.bsp.source_stable && sample.bsp.candidates_copied == 0);
    complete.put<std::uint32_t>(0x17000, 2);
    complete.put<std::uint32_t>(0x11000, 2);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.mesh_node.stable && !sample.target.mesh_node.sections_available);
    complete.put<std::uint32_t>(0x11000, 3);
    complete.put<std::uint32_t>(0x12000 + 24, 700);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.mesh_node.stable && !sample.target.mesh_node.sections_available);
    complete.put<std::uint32_t>(0x12000 + 24, 0);
    complete.put<std::uint32_t>(0xf000, 0);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.mesh_node.pool_scan_complete && !sample.target.mesh_node.stable);
    complete.put<std::uint32_t>(0xf000, 0x10e49d14);
    complete.put<std::uint32_t>(0x10f31bc0, 8193);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.mesh_node.pool_scan_truncated && !sample.target.mesh_node.stable);
    complete.put<std::uint32_t>(0x10f31bc0, 1);
    complete.put<std::uint32_t>(0xf010, 17);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.mesh_node.stable && sample.target.mesh_node.sections_truncated &&
           !sample.target.mesh_node.sections_available);
    complete.put<std::uint32_t>(0xf010, 3);
    complete.put<float>(0xf074, 5.0f);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(!sample.target.mesh_node.stable);
    complete.put<float>(0xf074, 0.0f);
    complete.put<std::uint32_t>(0x623c, 0x400000);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.asset.status == AssetStatus::DescriptorUnsupported && !sample.target.asset.name_available);
    std::uint32_t ignored = 0;
    assert(!ProbeOffset(0xfffffff0u, 0x20, ignored));
    assert(!ReadProbe(reader, 0xffffffffu, &ignored, sizeof(ignored)));
}
