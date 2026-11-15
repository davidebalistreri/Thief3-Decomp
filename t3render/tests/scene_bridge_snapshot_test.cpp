#include "../src/scene_bridge_snapshot.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>

using namespace t3render::scene_bridge;

struct Memory {
    std::map<std::uint32_t, std::uint8_t> bytes;
    unsigned owner_reads = 0;
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
        if (address >= 0x4000 && address < 0x4200) ++memory.owner_reads;
        auto* result = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0; i < count; ++i) {
            const auto found = memory.bytes.find(address + static_cast<std::uint32_t>(i));
            if (found == memory.bytes.end()) return false;
            result[i] = found->second;
        }
        return true;
    }
};

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
    complete.region(0x10f3eda4, 4); complete.region(0x10e72e48 + 0xb4, 4);
    complete.region(0x10f7b5c8 + 3 * 8, 8);
    complete.put<std::uint32_t>(0x10f3e4a4, 4); complete.put<std::uint32_t>(0x10f3e4a8, 4);
    complete.put<std::uint32_t>(0x8004, 0x7000); complete.put<std::uint32_t>(0x800c, 0xc000);
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
    reader.context = &complete;
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.object_scan_complete && sample.target.list_scan_complete && sample.target.entry_available);
    assert(sample.target.owner_class.stable && sample.target.owner_class.name_available);
    assert(sample.target.asset.status == AssetStatus::Candidate && sample.target.asset.stable);
    assert(std::strcmp(sample.target.asset.name, mesh_name) == 0);
    complete.put<std::uint32_t>(0x623c, 0x400000);
    sample = SampleProbe(reader, 42, 0x1000, 0);
    assert(sample.target.asset.status == AssetStatus::DescriptorUnsupported && !sample.target.asset.name_available);
    std::uint32_t ignored = 0;
    assert(!ProbeOffset(0xfffffff0u, 0x20, ignored));
    assert(!ReadProbe(reader, 0xffffffffu, &ignored, sizeof(ignored)));
}
