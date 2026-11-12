#include "../ir/vertex_declaration.hpp"
#include "../ir/vertex_declaration_registry.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace t3render::ir;

namespace {
constexpr std::uintptr_t kBase = 0x1000;
constexpr std::uint32_t kEnd = 0xffffffffu;
constexpr std::uint32_t kStream0 = 1u << 29;
constexpr std::uint32_t kPositionFloat3 = (2u << 29) | (2u << 16);
constexpr std::uint32_t kConst2 = (4u << 29) | (2u << 25) | 3u;
constexpr std::uint32_t kExt3 = (5u << 29) | (3u << 24) | 7u;

struct Memory {
    std::uintptr_t base = kBase;
    std::vector<unsigned char> bytes;
    std::size_t calls = 0;
    std::size_t fail_at = (std::numeric_limits<std::size_t>::max)();
    bool raises = false;
};
Memory MakeMemory(const std::vector<std::uint32_t>& words) {
    Memory memory;
    for (std::uint32_t value : words)
        for (unsigned shift = 0; shift < 32; shift += 8)
            memory.bytes.push_back(static_cast<unsigned char>(value >> shift));
    return memory;
}
bool ReadMemory(void* opaque, std::uintptr_t address, void* out, std::size_t size) {
    auto& memory = *static_cast<Memory*>(opaque);
    const std::size_t call = memory.calls++;
    if (memory.raises) throw std::runtime_error("reader failure");
    if (call == memory.fail_at) {
        if (size) static_cast<unsigned char*>(out)[0] = 0xaa; // partial data must not escape
        return false;
    }
    if (address < memory.base) return false;
    const auto offset = address - memory.base;
    if (offset > memory.bytes.size() || size > memory.bytes.size() - offset) return false;
    std::memcpy(out, memory.bytes.data() + offset, size);
    return true;
}

DeclarationSnapshot Complete(std::vector<std::uint32_t> words = {kStream0, kPositionFloat3, kEnd}) {
    return {DeclarationStatus::Complete, std::move(words)};
}

void ScannerFramesPayloadAndLimits() {
    // Constant and extension payloads may contain 0xffffffff. Only a token
    // boundary may terminate the declaration.
    const std::vector<std::uint32_t> words = {
        0, kStream0, kPositionFloat3, kConst2, kEnd, 2, 3, 4, 5, 6, kEnd, 8,
        kExt3, kEnd, 0x20000000, 0, (2u << 29) | (1u << 28) | (2u << 16), kEnd,
    };
    auto memory = MakeMemory(words);
    auto result = ScanDeclaration(kBase, ReadMemory, &memory, words.size());
    assert(result.status == DeclarationStatus::Complete && result.words == words && memory.calls == words.size());
    memory = MakeMemory(words);
    result = ScanDeclaration(kBase, ReadMemory, &memory, words.size() - 1);
    assert(result.status == DeclarationStatus::WordLimit && result.words.empty());
    memory = MakeMemory({kConst2, kEnd, 2});
    result = ScanDeclaration(kBase, ReadMemory, &memory, 16);
    assert(result.status == DeclarationStatus::Unreadable && result.words.empty());
    memory = MakeMemory(words); memory.fail_at = 5;
    result = ScanDeclaration(kBase, ReadMemory, &memory);
    assert(result.status == DeclarationStatus::Unreadable && result.words.empty());
    memory.raises = true;
    result = ScanDeclaration(kBase, ReadMemory, &memory);
    assert(result.status == DeclarationStatus::Unreadable && result.words.empty());

    for (auto invalid : {1u, 6u << 29, 7u << 29}) {
        memory = MakeMemory({invalid, kEnd});
        result = ScanDeclaration(kBase, ReadMemory, &memory);
        assert(result.status == DeclarationStatus::UnsupportedToken && result.words.empty());
    }
    assert(ScanDeclaration(0, ReadMemory, &memory).status == DeclarationStatus::NullAddress);
    assert(ScanDeclaration(kBase, nullptr, &memory).status == DeclarationStatus::InvalidConfiguration);
    assert(ScanDeclaration(kBase, ReadMemory, &memory, 0).status == DeclarationStatus::InvalidConfiguration);
    assert(ScanDeclaration(kBase, ReadMemory, &memory, 4097).status == DeclarationStatus::InvalidConfiguration);
    const auto before = memory.calls;
    assert(ScanDeclaration((std::numeric_limits<std::uintptr_t>::max)() - 1, ReadMemory, &memory).status ==
           DeclarationStatus::AddressOverflow);
    assert(memory.calls == before);
}

void Register(VertexDeclarationRegistry& registry, std::uint64_t device, std::uint32_t handle,
              DeclarationSnapshot snapshot = Complete()) {
    const auto create = registry.BeginCreate(device);
    assert(create && registry.EndCreate(create, true, true, handle, std::move(snapshot)));
}
ShaderObservation Observe(VertexDeclarationRegistry& registry, std::uint64_t device, std::uint32_t value) {
    const auto draw = registry.BeginDraw(device);
    assert(draw);
    auto observation = registry.Observe(draw, value);
    assert(registry.End(draw));
    return observation;
}

void DeviceLifetimeReuseAndFailures() {
    VertexDeclarationRegistry registry;
    const auto a = registry.OpenDevice(), b = registry.OpenDevice();
    assert(a && b && a != b);
    Register(registry, a, 0x44);
    Register(registry, b, 0x44, Complete({kStream0, kEnd}));
    auto first = Observe(registry, a, 0x44);
    assert(first.status == ShaderObservationStatus::Candidate && first.words.size() == 3 && first.generation);
    assert(Observe(registry, b, 0x44).words.size() == 2);
    assert(Observe(registry, a, 0x42).status == ShaderObservationStatus::Unknown);

    // Failed Create does not read/trust the output or destroy an existing entry.
    auto create = registry.BeginCreate(a);
    assert(registry.EndCreate(create, false, false, 0, {}));
    assert(Observe(registry, a, 0x44).generation == first.generation);
    Register(registry, a, 0x44); // duplicate success is ambiguous, not a replacement
    auto collision = Observe(registry, a, 0x44);
    assert(collision.status == ShaderObservationStatus::Unavailable && collision.words.empty() &&
           collision.generation != first.generation);
    auto deletion = registry.BeginDelete(a, 0x44);
    assert(deletion && registry.EndDelete(deletion, false));
    assert(Observe(registry, a, 0x44).status == ShaderObservationStatus::Unavailable);
    Register(registry, a, 0x44); // a failed Delete cannot resolve the collision
    assert(Observe(registry, a, 0x44).status == ShaderObservationStatus::Unavailable);
    deletion = registry.BeginDelete(a, 0x44);
    assert(deletion && registry.EndDelete(deletion, true));
    assert(Observe(registry, a, 0x44).status == ShaderObservationStatus::Unknown);
    Register(registry, a, 0x44);
    auto second = Observe(registry, a, 0x44);
    assert(second.status == ShaderObservationStatus::Candidate && second.generation != first.generation);

    auto reset = registry.BeginReset(a);
    assert(reset && registry.End(reset)); // even a failed native Reset invalidates
    assert(Observe(registry, a, 0x44).status == ShaderObservationStatus::Unavailable);
    Register(registry, a, 0x44);
    assert(Observe(registry, a, 0x44).status == ShaderObservationStatus::Unavailable);
    deletion = registry.BeginDelete(a, 0x44);
    assert(deletion && registry.EndDelete(deletion, true));
    Register(registry, a, 0x44);
    assert(Observe(registry, a, 0x44).generation != second.generation);
    auto set = registry.BeginSet(a);
    assert(set && registry.End(set));
    auto stale = registry.BeginDraw(a);
    assert(stale);
    registry.CloseDevice(a);
    const auto replacement = registry.OpenDevice();
    assert(replacement && replacement != a && !registry.End(stale));
    assert(Observe(registry, replacement, 0x44).status == ShaderObservationStatus::Unknown);
    assert(Observe(registry, b, 0x44).status == ShaderObservationStatus::Candidate);
}

void UnavailableAndCapacityFailClosed() {
    VertexDeclarationRegistry registry({2, 1, 4, 4});
    const auto a = registry.OpenDevice(), b = registry.OpenDevice();
    assert(a && b && !registry.OpenDevice());
    Register(registry, a, 5, {DeclarationStatus::Unreadable, {0x11111111}});
    auto unavailable = Observe(registry, a, 5);
    assert(unavailable.status == ShaderObservationStatus::Unavailable && unavailable.words.empty());
    auto deletion = registry.BeginDelete(a, 5);
    assert(deletion && registry.EndDelete(deletion, true));
    Register(registry, a, 5);
    assert(Observe(registry, a, 5).status == ShaderObservationStatus::Candidate);
    auto create = registry.BeginCreate(a);
    assert(!registry.EndCreate(create, true, true, 6, Complete())); // handle capacity
    assert(!registry.BeginDraw(a));

    create = registry.BeginCreate(b);
    assert(!registry.EndCreate(create, true, false, 0, Complete())); // unknown successful output
    assert(!registry.BeginDraw(b));
    registry.CloseDevice(a);
    const auto c = registry.OpenDevice();
    assert(c && c != a);
    create = registry.BeginCreate(c);
    assert(!registry.EndCreate(create, true, true, 3, Complete({1, 2, 3, 4, kEnd})));
    assert(!registry.BeginDraw(c));
}

void TombstonesReleaseBoundedStorage() {
    VertexDeclarationRegistry registry({1, 2, 4, 4});
    const auto device = registry.OpenDevice();
    assert(device);
    auto full = Complete({kStream0, kPositionFloat3, 0, kEnd});
    Register(registry, device, 1, full);
    Register(registry, device, 1, full); // collision frees the previous words
    assert(Observe(registry, device, 1).status == ShaderObservationStatus::Unavailable);
    Register(registry, device, 2, full); // the device's full word budget is reusable
    assert(Observe(registry, device, 2).status == ShaderObservationStatus::Candidate);
    const auto reset = registry.BeginReset(device);
    assert(reset && registry.End(reset));
    const auto deletion = registry.BeginDelete(device, 1);
    assert(deletion && registry.EndDelete(deletion, true));
    Register(registry, device, 3, full); // Reset freed bytes; successful Delete freed a slot
    assert(Observe(registry, device, 2).status == ShaderObservationStatus::Unavailable);
    assert(Observe(registry, device, 3).status == ShaderObservationStatus::Candidate);

    VertexDeclarationRegistry bounded({1, 2, 3, 6});
    const auto other = bounded.OpenDevice();
    auto overallocated = Complete();
    overallocated.words.reserve(100000); // spare caller capacity must not enter registry
    Register(bounded, other, 1, std::move(overallocated));
    Register(bounded, other, 2);
    assert(Observe(bounded, other, 1).status == ShaderObservationStatus::Candidate);
    assert(Observe(bounded, other, 2).status == ShaderObservationStatus::Candidate);
}

void OverlapInvalidatesOnlyAffectedDevice() {
    VertexDeclarationRegistry registry;
    const auto a = registry.OpenDevice(), b = registry.OpenDevice();
    Register(registry, a, 42);
    Register(registry, b, 42);
    const auto draw = registry.BeginDraw(a);
    assert(draw && registry.Observe(draw, 42).status == ShaderObservationStatus::Candidate);
    ShaderTicket deletion;
    std::thread thread([&] { deletion = registry.BeginDelete(a, 42); });
    thread.join();
    assert(!deletion && !registry.End(draw)); // observation must be discarded
    assert(!registry.BeginDraw(a));
    assert(Observe(registry, b, 42).status == ShaderObservationStatus::Candidate);

    const auto create = registry.BeginCreate(b);
    assert(create);
    assert(!registry.BeginReset(b)); // create/reset overlap is unsafe too
    assert(!registry.EndCreate(create, true, true, 43, Complete()));
    assert(!registry.BeginDraw(b));
}

void InvalidProtocolAndEscapeAreConservative() {
    VertexDeclarationRegistry registry;
    const auto a = registry.OpenDevice();
    Register(registry, a, 1);
    const auto draw = registry.BeginDraw(a);
    assert(draw && registry.End(draw));
    assert(!registry.End(draw)); // stale completion cannot attach to a later draw
    assert(!registry.BeginDraw(a));
    const auto b = registry.OpenDevice();
    Register(registry, b, 1);
    registry.Invalidate(b); // e.g. unobserved device alias escaped
    assert(!registry.BeginDraw(b));
}
} // namespace

int main() {
    ScannerFramesPayloadAndLimits();
    DeviceLifetimeReuseAndFailures();
    UnavailableAndCapacityFailClosed();
    TombstonesReleaseBoundedStorage();
    OverlapInvalidatesOnlyAffectedDevice();
    InvalidProtocolAndEscapeAreConservative();
}
