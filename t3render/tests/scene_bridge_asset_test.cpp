#include "../src/scene_bridge_asset.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>

using namespace t3render::scene_bridge;

struct Memory {
    std::map<std::uint32_t, std::uint8_t> bytes;
    unsigned name_reads = 0;
    bool change_name = false;
    template<class T> void put(std::uint32_t address, T value) {
        std::uint8_t data[sizeof(value)];
        std::memcpy(data, &value, sizeof(value));
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[address + static_cast<std::uint32_t>(i)] = data[i];
    }
    static bool read(void* context, std::uint32_t address, void* dest, std::size_t size) noexcept {
        auto& m = *static_cast<Memory*>(context);
        if (address == 0x5004 && ++m.name_reads == 2 && m.change_name) m.bytes[0x5004] = 'X';
        auto* out = static_cast<std::uint8_t*>(dest);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = m.bytes.find(address + static_cast<std::uint32_t>(i));
            if (it == m.bytes.end()) return false;
            out[i] = it->second;
        }
        return true;
    }
};

Memory fixture(bool indirect = false) {
    Memory m;
    m.put<std::uint32_t>(0x10f3eda4, 0x6000);
    m.put<std::uint32_t>(0x6008, 0x6100);
    m.put<std::uint32_t>(0x6100 + 0x2b * 4, 0x6200);
    m.put<std::uint32_t>(0x6200, 0x10e72e48);
    m.put<std::uint32_t>(0x10e72e48 + 0xb4, 0x10aeecc0);
    m.put<std::uint32_t>(0x6234, 1); m.put<std::uint32_t>(0x6238, 4); m.put<std::uint32_t>(0x623c, 0);
    m.put<std::uint32_t>(0x101c, 0); m.put<std::uint32_t>(0x1028, 0x2000);
    m.put<std::uint16_t>(0x2006, 3);
    const std::uint32_t slot = 0x10f7b5c8 + 3 * 8;
    m.put<std::uint16_t>(slot, 0); m.put<std::uint16_t>(slot + 2, indirect ? 0x2b : 0x802b);
    m.put<std::uint32_t>(slot + 4, indirect ? 0x7000 : 0x3000);
    if (indirect) m.put<std::uint32_t>(0x7000, 0x3000);
    m.put<std::uint32_t>(0x10f3e4a0, 0x8000); m.put<std::uint32_t>(0x10f3e4a4, 3);
    m.put<std::uint32_t>(0x10f3e4a8, 3); m.put<std::uint32_t>(0x8000, 0);
    m.put<std::uint32_t>(0x8004, 0); m.put<std::uint32_t>(0x8008, 0x3000);
    m.put<std::uint32_t>(0x3000, 0x10e4dfa8); m.put<std::uint32_t>(0x3004, 2);
    m.put<std::uint32_t>(0x3058, 0x4000); m.put<std::uint32_t>(0x4000, 0x10ec0e20);
    m.put<std::uint32_t>(0x4008, 0x5004); m.put<std::uint32_t>(0x5000, 8);
    const char text[] = "INNbed1A";
    for (std::size_t i = 0; i < sizeof(text); ++i) m.put<std::uint8_t>(0x5004 + static_cast<std::uint32_t>(i), text[i]);
    return m;
}

AssetCandidate sample(Memory& m) { return InspectAssetCandidate({&m, Memory::read}, 0x1000); }

int main() {
    for (const bool indirect : {false, true}) {
        auto m = fixture(indirect);
        const auto a = sample(m);
        assert(a.status == AssetStatus::Candidate && a.stable && a.name_available);
        assert(a.descriptor_match && a.local_property_found && a.proxy_live_member &&
               a.proxy_vtable_match && a.resource_vtable_match && std::strcmp(a.name, "INNbed1A") == 0);
        assert(a.audit.available && a.audit.descriptor == 0x6200 && a.audit.descriptor_count == 1 &&
               a.audit.descriptor_size == 4 && a.audit.descriptor_flags == 0);
        assert(a.audit.property_hash == 0x2000 && a.audit.bucket_head == 3 && a.audit.slot_index == 3 &&
               a.audit.slot_id_flags == (indirect ? 0x2b : 0x802b) &&
               a.audit.slot_value == (indirect ? 0x7000u : 0x3000u));
        assert(a.audit.chain_count == 1 && a.audit.chain[0].index == 3 &&
               a.audit.chain[0].next == 0 && a.audit.chain[0].value == a.audit.slot_value);
        assert(a.audit.proxy == 0x3000 && a.audit.object_index == 2 && a.audit.objects_count == 3 &&
               a.audit.proxy_index == 2 && a.audit.resource == 0x4000 &&
               a.audit.name_address == 0x5004 && a.audit.name_length == 8);
    }
    {
        auto m = fixture();
        m.put<std::uint16_t>(0x2006, 2);
        const std::uint32_t slot = 0x10f7b5c8 + 2 * 8;
        m.put<std::uint16_t>(slot, 3); m.put<std::uint16_t>(slot + 2, 0x8023);
        m.put<std::uint32_t>(slot + 4, 7);
        const auto a = sample(m);
        assert(a.status == AssetStatus::Candidate && a.audit.chain_count == 2 &&
               a.audit.bucket_head == 2 && a.audit.chain[0].index == 2 && a.audit.chain[0].next == 3 &&
               a.audit.chain[1].index == 3 && a.audit.chain[1].next == 0);
    }
    {
        auto m = fixture(); m.put<std::uint32_t>(0x623c, 0x400000);
        const auto a = sample(m);
        assert(a.status == AssetStatus::DescriptorUnsupported && !a.name_available);
    }
    {
        auto m = fixture(); m.put<std::uint32_t>(0x6238, 8);
        assert(sample(m).status == AssetStatus::DescriptorUnsupported);
    }
    {
        auto m = fixture(); m.put<std::uint16_t>(0x2006, 0);
        assert(sample(m).status == AssetStatus::LocalPropertyMissing);
    }
    {
        auto m = fixture(); m.put<std::uint16_t>(0x10f7b5c8 + 3 * 8, 3);
        assert(sample(m).status == AssetStatus::PropertyMalformed);
    }
    {
        auto m = fixture();
        const std::uint32_t slot = 0x10f7b5c8 + 3 * 8;
        m.put<std::uint16_t>(slot, 4); m.put<std::uint16_t>(slot + 2, 0x802c);
        m.put<std::uint16_t>(slot + 8, 0); m.put<std::uint16_t>(slot + 10, 0x802b);
        m.put<std::uint32_t>(slot + 12, 0x3000);
        assert(sample(m).status == AssetStatus::PropertyMalformed);
    }
    {
        auto m = fixture(); m.put<std::uint32_t>(0x8004, 0x3000);
        assert(sample(m).status == AssetStatus::ObjectNotUnique);
    }
    {
        auto m = fixture(); m.put<std::uint32_t>(0x3000, 0);
        assert(sample(m).status == AssetStatus::ProxyMismatch);
    }
    {
        auto m = fixture(); m.put<std::uint32_t>(0x4000, 0);
        assert(sample(m).status == AssetStatus::ResourceMismatch);
    }
    {
        auto m = fixture(); m.change_name = true;
        const auto a = sample(m);
        assert(a.status == AssetStatus::Unknown && !a.stable && !a.name_available && !a.audit.available);
    }
    {
        auto m = fixture(); m.put<std::uint8_t>(0x5004 + 8, '!');
        assert(sample(m).status == AssetStatus::Unreadable);
    }
}
