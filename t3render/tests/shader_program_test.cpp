#include "../ir/shader_program.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using t3render::ir::ScanVertexShader11;
using t3render::ir::ShaderProgramStatus;

namespace {
struct Memory {
    std::uintptr_t base = 0x1000;
    std::vector<std::uint32_t> words;
    unsigned reads = 0;
    bool throws = false;
};

bool Read(void* context, std::uintptr_t address, void* out, std::size_t length) {
    auto& mem = *static_cast<Memory*>(context);
    ++mem.reads;
    if (mem.throws) throw 1;
    if (address < mem.base || length != 4 || (address - mem.base) % 4) return false;
    const auto index = (address - mem.base) / 4;
    if (index >= mem.words.size()) return false;
    // Supply explicit little-endian bytes, independent of the host.
    const auto word = mem.words[index];
    unsigned char bytes[] = {static_cast<unsigned char>(word), static_cast<unsigned char>(word >> 8),
                             static_cast<unsigned char>(word >> 16), static_cast<unsigned char>(word >> 24)};
    std::memcpy(out, bytes, sizeof(bytes));
    return true;
}

void ExpectFailure(Memory& mem, ShaderProgramStatus expected, std::size_t limit = 64) {
    const auto result = ScanVertexShader11(mem.base, Read, &mem, limit);
    assert(result.status == expected && result.words.empty());
}

void FixtureProgram() {
    // vs_1_1: mov oPos, v0; mov oD0, c0, as used by the reset fixture.
    Memory mem{0x1000, {0xfffe0101, 1, 0xc00f0000, 0x90e40000,
                        1, 0xd00f0000, 0xa0e40000, 0x0000ffff}};
    const auto result = ScanVertexShader11(mem.base, Read, &mem, 8);
    assert(result.status == ShaderProgramStatus::Complete && result.words == mem.words);
    assert(mem.reads == mem.words.size());
    ExpectFailure(mem, ShaderProgramStatus::WordLimit, 7);
}

void EmbeddedEndWordsAreNotTerminators() {
    // A comment and a DEF may contain END or version-looking data. They must
    // be skipped according to their actual lengths, not searched by value.
    Memory mem{0x1000, {0xfffe0101, 0x0002fffe, 0x0000ffff, 0xfffe0101,
                        81, 0xa00f0000, 0x0000ffff, 0, 0xffffffff, 0x3f800000,
                        0x0000ffff}};
    const auto result = ScanVertexShader11(mem.base, Read, &mem, 64);
    assert(result.status == ShaderProgramStatus::Complete && result.words == mem.words);
    assert(mem.reads == mem.words.size());
    mem.words.pop_back();
    ExpectFailure(mem, ShaderProgramStatus::Unreadable);
}

void BadInputFailsClosed() {
    Memory mem{0x1000, {0xfffe0101, 1, 0xc00f0000, 0x90e40000, 0x0000ffff}};
    auto invalid = ScanVertexShader11(mem.base, nullptr, &mem, 64);
    assert(invalid.status == ShaderProgramStatus::InvalidConfiguration && invalid.words.empty());
    invalid = ScanVertexShader11(mem.base, Read, &mem, 0);
    assert(invalid.status == ShaderProgramStatus::InvalidConfiguration && invalid.words.empty());
    invalid = ScanVertexShader11(mem.base, Read, &mem, 4097);
    assert(invalid.status == ShaderProgramStatus::InvalidConfiguration && invalid.words.empty());
    invalid = ScanVertexShader11(0, Read, &mem, 64);
    assert(invalid.status == ShaderProgramStatus::NullAddress && invalid.words.empty());

    mem.words[0] = 0xfffe0200;
    ExpectFailure(mem, ShaderProgramStatus::UnsupportedVersion);
    mem.words[0] = 0xffff0101; // ps_1_1 is not a vertex program
    ExpectFailure(mem, ShaderProgramStatus::UnsupportedVersion);
    mem.words[0] = 0xfffe0101;
    mem.words[1] = 0x01000001; // no instruction-length token in VS 1.1
    ExpectFailure(mem, ShaderProgramStatus::UnsupportedInstruction);
    mem.words[1] = 1;
    mem.words[2] = 0x0000ffff; // not a register parameter
    ExpectFailure(mem, ShaderProgramStatus::InvalidParameter);
    mem.words[2] = 0xc00f0000;
    mem.words[3] = 0; // not a register parameter
    ExpectFailure(mem, ShaderProgramStatus::InvalidParameter);
    mem.words[3] = 0x90e40000;
    mem.throws = true;
    ExpectFailure(mem, ShaderProgramStatus::Unreadable);
    mem.throws = false;
    mem.base = (std::numeric_limits<std::uintptr_t>::max)() - 3;
    ExpectFailure(mem, ShaderProgramStatus::AddressOverflow);
}

void CountsAndBounds() {
    // Exercise one instance of every accepted operand count, and ensure a
    // malicious comment length stops at the declared word budget.
    Memory mem{0x1000, {0xfffe0101, 0, 1, 0x80000000, 0x80000000,
                        2, 0x80000000, 0x80000000, 0x80000000,
                        4, 0x80000000, 0x80000000, 0x80000000, 0x80000000,
                        0x0000ffff}};
    auto result = ScanVertexShader11(mem.base, Read, &mem, 64);
    assert(result.status == ShaderProgramStatus::Complete && result.words == mem.words);
    mem.words = {0xfffe0101, 0x7ffffffe, 0, 0, 0, 0};
    ExpectFailure(mem, ShaderProgramStatus::WordLimit, 6);
}
} // namespace

int main() {
    FixtureProgram();
    EmbeddedEndWordsAreNotTerminators();
    BadInputFailsClosed();
    CountsAndBounds();
}
