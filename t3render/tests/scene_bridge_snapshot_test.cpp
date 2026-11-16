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
    complete.region(0x3100, 16); complete.region(0xf000, 0xc0); complete.region(0xf100, 32);
    complete.region(0x10f31bb8, 12);
    complete.region(0x11000, 40); complete.region(0x12000, 3 * 36);
    complete.region(0x13000, 32); complete.region(0x14000, 32);
    complete.region(0x14100, 32); complete.region(0x14200, 32);
    complete.region(0x10f2c740, 4); complete.region(0x15000, 32); complete.region(0x16000, 0x58);
    complete.region(0x17000, 4 + 2 * 0x1ac); complete.region(0x18000, 4 + 2 * 36);
    complete.region(0x19000, 32); complete.region(0x19100, 32);
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
    assert(sample.bsp.source_stable && sample.bsp.block_count == 2 && sample.bsp.type5_seen == 1 &&
           sample.bsp.member_count == 1 && sample.bsp.candidates_copied == 1);
    const auto& bsp = sample.bsp.candidates[0];
    assert(bsp.stable && bsp.source_index == 1 && bsp.list_index == 1 &&
           bsp.vertex_count == 60 && bsp.index_count == 114 && bsp.sections_available &&
           bsp.sections_copied == 2 && std::strcmp(bsp.sections[0].material_name, "wall") == 0);
    // The live path checks only the visible actor's identity; it must not scan
    // each unrelated UObject by name on every game frame.
    auto large=complete;
    constexpr std::uint32_t extra=1000, table_base=0x200000, object_base=0x300000;
    large.region(table_base,(extra+4)*4);
    for (std::uint32_t i=0;i<4;++i) {
        std::uint32_t value=0;
        assert(Memory::read(&large,0x8000+i*4,&value,sizeof(value)));
        large.put<std::uint32_t>(table_base+i*4,value);
    }
    for (std::uint32_t i=0;i<extra;++i) {
        const auto object=object_base+i*64;large.region(object,0x24);
        large.put<std::uint32_t>(table_base+(i+4)*4,object);
    }
    large.put<std::uint32_t>(0x10f3e4a0,table_base);
    large.put<std::uint32_t>(0x10f3e4a4,extra+4);large.put<std::uint32_t>(0x10f3e4a8,extra+4);
    large.put<std::uint32_t>(0x10efef54,0);
    reader.context=&large;large.read_calls=0;
    auto full=SampleProbe(reader,42,0x1000,0x109271ae);const auto full_reads=large.read_calls;
    large.read_calls=0;auto live=SampleLiveProbe(reader,42,0x1000,0x109271ae);const auto live_reads=large.read_calls;
    assert(full.target.object_scan_complete && full.target.entry_available);
    assert(live.target.visible_list_only && !live.target.object_scan_complete && live.target.list_scan_complete &&
           live.target.list_occurrences==1 && live.target.entry_available && live.target.owner_class.stable &&
           live.target.asset.status==AssetStatus::Candidate && live.target.mesh_node.stable && live.bsp.source_stable);
    assert(live_reads*2<full_reads);
    std::printf("probe_read_calls full=%u live=%u unrelated_objects=%u\n",full_reads,live_reads,extra);
    // Another globally named actor outside the visible list is not ruled out
    // by the fast path; its result must explicitly retain the weaker scope.
    large.put<std::uint32_t>(object_base+0x20,(1521u<<16)|7);
    full=SampleProbe(reader,42,0x1000,0x109271ae);
    live=SampleLiveProbe(reader,42,0x1000,0x109271ae);
    assert(full.target.actors_named_target==2 && !full.target.entry_available);
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
