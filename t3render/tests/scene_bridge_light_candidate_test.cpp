#include "../src/scene_bridge_light_candidate.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>

using namespace t3render::scene_bridge;

struct Memory {
    std::map<std::uint32_t, std::uint8_t> bytes;
    std::uint32_t mutate_trigger = 0, mutate_address = 0, mutate_value = 0;
    unsigned trigger_occurrence = 1, triggers = 0, reads = 0;
    template<class T> void put(std::uint32_t address, T value) {
        std::uint8_t data[sizeof(value)]; std::memcpy(data, &value, sizeof(value));
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[address + i] = data[i];
    }
    static bool read(void* context, std::uint32_t address, void* output, std::size_t count) noexcept {
        auto& m = *static_cast<Memory*>(context); ++m.reads;
        for (std::size_t i = 0; i < count; ++i)
            if (!m.bytes.count(address + i)) return false;
        auto* out = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0; i < count; ++i) out[i] = m.bytes[address + i];
        if (address == m.mutate_trigger && ++m.triggers == m.trigger_occurrence)
            m.put<std::uint32_t>(m.mutate_address, m.mutate_value);
        return true;
    }
};

Memory Fixture() {
    Memory m;
    m.put<std::uint32_t>(0x10b8, 0x2000);
    m.put<std::uint32_t>(0x2000, 0x10e4aa80);
    m.put<std::uint32_t>(0x2054, 0x1000);
    for (unsigned i = 0; i < 3; ++i) {
        m.put<std::uint32_t>(0x2024 + 4*i, 0x100 + i);
        m.put<std::uint32_t>(0x2034 + 4*i, 0x200 + i);
    }
    for (unsigned i = 0; i < 15; ++i) m.put<std::uint32_t>(0x2058 + 4*i, 0x300 + i);
    return m;
}

bool Empty(const LightCandidateWords& words) {
    for (auto v : words.direction_24) if (v) return false;
    for (auto v : words.position_34) if (v) return false;
    for (auto v : words.words_58_to_90) if (v) return false;
    return true;
}

int main() {
    Memory m = Fixture(); ProbeReader reader{&m, Memory::read};
    auto candidate = InspectLightCandidate(reader, 0x1000);
    assert(candidate.status == LightCandidateStatus::Candidate && m.reads == 13);
    assert(candidate.words.direction_24[2] == 0x102 && candidate.words.position_34[1] == 0x201 &&
           candidate.words.words_58_to_90[14] == 0x30e);
    assert(InspectLightCandidate({}, 0x1000).status == LightCandidateStatus::InvalidInput);
    assert(InspectLightCandidate(reader, 0).status == LightCandidateStatus::InvalidInput);

    m = Fixture(); m.put<std::uint32_t>(0x10b8, 0);
    assert(InspectLightCandidate(reader, 0x1000).status == LightCandidateStatus::NoLight);
    m = Fixture(); m.bytes.erase(0x2080);
    candidate = InspectLightCandidate(reader, 0x1000);
    assert(candidate.status == LightCandidateStatus::Unreadable && Empty(candidate.words));
    m = Fixture(); m.put<std::uint32_t>(0x2000, 0x1234);
    candidate = InspectLightCandidate(reader, 0x1000);
    assert(candidate.status == LightCandidateStatus::ObjectMismatch && Empty(candidate.words));
    m = Fixture(); m.put<std::uint32_t>(0x2054, 0x9999);
    assert(InspectLightCandidate(reader, 0x1000).status == LightCandidateStatus::ObjectMismatch);

    // Mutate each selected group, vtable, backlink, or actor link between
    // reads; no partial words may escape on a rejected sample.
    for (const auto address : {0x2024u, 0x2034u, 0x2058u, 0x2000u, 0x2054u, 0x10b8u}) {
        m = Fixture(); m.mutate_trigger = 0x2058; m.mutate_address = address; m.mutate_value = 0xabcdef01;
        candidate = InspectLightCandidate(reader, 0x1000);
        assert(candidate.status == LightCandidateStatus::Changed && Empty(candidate.words));
    }
    // An actor link changed only after the second sample is also rejected.
    m = Fixture(); m.mutate_trigger = 0x2058; m.trigger_occurrence = 2;
    m.mutate_address = 0x10b8; m.mutate_value = 0x3000;
    assert(InspectLightCandidate(reader, 0x1000).status == LightCandidateStatus::Changed);
    m = Fixture(); m.put<std::uint32_t>(0x10b8, 0xfffffff0u);
    assert(InspectLightCandidate(reader, 0x1000).status == LightCandidateStatus::Unreadable);
    assert(InspectLightCandidate(reader, 0xfffffff0u).status == LightCandidateStatus::Unreadable);
}
