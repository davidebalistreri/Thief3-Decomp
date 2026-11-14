#include "shader_program.hpp"

#include <limits>

namespace t3render::ir {
namespace {
constexpr std::size_t kAbsoluteWordLimit = 4096;
constexpr std::uint32_t kVs11 = 0xfffe0101u;
constexpr std::uint32_t kEnd = 0x0000ffffu;
constexpr std::uint32_t kComment = 0x0000fffeu;
constexpr std::uint32_t kDef = 81;

// Number of parameter tokens, excluding the opcode. This is framing only:
// it does not validate registers, modifiers, instruction legality or execution.
int ParameterCount(std::uint32_t opcode) noexcept {
    switch (opcode) {
    case 0: return 0; // nop
    case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79:
        return 2;
    case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13:
    case 17: case 20: case 21: case 22: case 23: case 24:
        return 3;
    case 4: case 18: return 4;
    case kDef: return 5; // destination followed by four arbitrary float bits
    default: return -1;
    }
}
} // namespace

ShaderProgramSnapshot ScanVertexShader11(std::uintptr_t address, DeclarationReader reader,
                                         void* context, std::size_t max_words) noexcept {
    ShaderProgramSnapshot result;
    if (!reader || !max_words || max_words > kAbsoluteWordLimit) {
        result.status = ShaderProgramStatus::InvalidConfiguration;
        return result;
    }
    if (!address) { result.status = ShaderProgramStatus::NullAddress; return result; }

    auto fail = [&](ShaderProgramStatus status) {
        result.words.clear();
        result.status = status;
    };
    try {
        auto read_word = [&]() -> bool {
            if (result.words.size() >= max_words) { fail(ShaderProgramStatus::WordLimit); return false; }
            const std::size_t offset = result.words.size() * 4;
            const auto limit = (std::numeric_limits<std::uintptr_t>::max)();
            if (offset > limit - address || address + offset > limit - 3) {
                fail(ShaderProgramStatus::AddressOverflow); return false;
            }
            unsigned char bytes[4]{};
            bool copied = false;
            try { copied = reader(context, address + offset, bytes, sizeof(bytes)); }
            catch (...) { copied = false; }
            if (!copied) { fail(ShaderProgramStatus::Unreadable); return false; }
            result.words.push_back(std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
                                   (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24));
            return true;
        };
        if (!read_word()) return result;
        result.first_word_known = true;
        result.first_word = result.words.back();
        if (result.words.back() != kVs11) { fail(ShaderProgramStatus::UnsupportedVersion); return result; }
        while (read_word()) {
            const std::uint32_t token = result.words.back();
            if (token == kEnd) { result.status = ShaderProgramStatus::Complete; return result; }
            if ((token & 0xffffu) == kComment && !(token & 0x80000000u)) {
                const std::size_t count = (token >> 16) & 0x7fffu;
                for (std::size_t i = 0; i < count; ++i) if (!read_word()) return result;
                continue;
            }
            // VS 1.1 has no instruction-length field or coissue flag. Refuse
            // modified/reserved opcode tokens instead of guessing their size.
            const int count = ParameterCount(token);
            if (count < 0) { fail(ShaderProgramStatus::UnsupportedInstruction); return result; }
            for (int i = 0; i < count; ++i) {
                if (!read_word()) return result;
                if ((token != kDef || i == 0) && !(result.words.back() & 0x80000000u)) {
                    fail(ShaderProgramStatus::InvalidParameter); return result;
                }
            }
        }
    } catch (...) { fail(ShaderProgramStatus::AllocationFailure); }
    return result;
}

} // namespace t3render::ir
