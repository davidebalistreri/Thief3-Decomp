#pragma once

// Bounded raw diagnostics for one independently identified live actor. This
// is not a light collector, a frame-synchronization claim, or a SceneIR value.
#include "scene_bridge_snapshot.hpp"

#include <cstdint>
#include <cstring>

namespace t3render::scene_bridge {

enum class LightCandidateStatus { InvalidInput, NoLight, Unreadable, ObjectMismatch, Changed, Candidate };

// Exact 32-bit words, not interpreted physical quantities. No process address
// is retained in the result. Offsets are for the validated x86 executable only.
struct LightCandidateWords {
    std::uint32_t direction_24[3]{};
    std::uint32_t position_34[3]{};
    std::uint32_t words_58_to_90[15]{};
};
struct LightCandidate {
    LightCandidateStatus status = LightCandidateStatus::InvalidInput;
    LightCandidateWords words{}; // populated only for Candidate
};

namespace light_candidate_detail {
constexpr std::uint32_t kLightVtable = 0x10e4aa80;
struct Sample {
    std::uint32_t vtable = 0, owner = 0;
    LightCandidateWords words{};
};
inline bool ReadSample(const ProbeReader& reader, std::uint32_t light, Sample& sample) noexcept {
    return ReadProbeBytesOffset(reader, light, 0, &sample.vtable, sizeof(sample.vtable)) &&
        ReadProbeBytesOffset(reader, light, 0x54, &sample.owner, sizeof(sample.owner)) &&
        ReadProbeBytesOffset(reader, light, 0x24, sample.words.direction_24, sizeof(sample.words.direction_24)) &&
        ReadProbeBytesOffset(reader, light, 0x34, sample.words.position_34, sizeof(sample.words.position_34)) &&
        ReadProbeBytesOffset(reader, light, 0x58, sample.words.words_58_to_90, sizeof(sample.words.words_58_to_90));
}
inline bool SameWords(const LightCandidateWords& a, const LightCandidateWords& b) noexcept {
    return std::memcmp(a.direction_24, b.direction_24, sizeof(a.direction_24)) == 0 &&
        std::memcmp(a.position_34, b.position_34, sizeof(a.position_34)) == 0 &&
        std::memcmp(a.words_58_to_90, b.words_58_to_90, sizeof(a.words_58_to_90)) == 0;
}
} // namespace light_candidate_detail

// The caller must establish actor identity and lifetime independently, use a
// fault-safe reader, and run only for the build checked by the existing probe.
// Matching reads cannot exclude ABA/reuse or prove visibility, complete light
// coverage, units, physical meaning, or membership of a Present interval.
inline LightCandidate InspectLightCandidate(const ProbeReader& reader, std::uint32_t actor) noexcept {
    using namespace light_candidate_detail;
    LightCandidate result;
    if (!reader.read || !actor) return result;
    const auto first_link = ReadProbeOffset(reader, actor, 0xb8);
    if (!first_link.available) { result.status = LightCandidateStatus::Unreadable; return result; }
    if (!first_link.value) { result.status = LightCandidateStatus::NoLight; return result; }
    Sample first{}, second{};
    if (!ReadSample(reader, first_link.value, first)) { result.status = LightCandidateStatus::Unreadable; return result; }
    if (first.vtable != kLightVtable || first.owner != actor) {
        result.status = LightCandidateStatus::ObjectMismatch; return result;
    }
    const auto middle_link = ReadProbeOffset(reader, actor, 0xb8);
    if (!middle_link.available) { result.status = LightCandidateStatus::Unreadable; return result; }
    if (middle_link.value != first_link.value) { result.status = LightCandidateStatus::Changed; return result; }
    if (!ReadSample(reader, first_link.value, second)) { result.status = LightCandidateStatus::Unreadable; return result; }
    const auto last_link = ReadProbeOffset(reader, actor, 0xb8);
    if (!last_link.available) { result.status = LightCandidateStatus::Unreadable; return result; }
    if (last_link.value != first_link.value || second.vtable != first.vtable || second.owner != first.owner ||
        !SameWords(first.words, second.words)) { result.status = LightCandidateStatus::Changed; return result; }
    result.status = LightCandidateStatus::Candidate;
    result.words = first.words;
    return result;
}

} // namespace t3render::scene_bridge
