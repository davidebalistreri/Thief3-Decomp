#include "vertex_declaration.hpp"

#include <limits>

namespace t3render::ir {
namespace {
constexpr std::size_t kAbsoluteWordLimit = 4096;
constexpr std::uint32_t kEnd = 0xffffffffu;

std::uint32_t LittleEndian(const unsigned char* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
bool Read(DeclarationReader reader, void* context, std::uintptr_t address, void* out, std::size_t size) noexcept {
    try { return reader(context, address, out, size); }
    catch (...) { return false; }
}
} // namespace

DeclarationSnapshot ScanDeclaration(std::uintptr_t address, DeclarationReader reader, void* context,
                                    std::size_t max_words) noexcept {
    DeclarationSnapshot result;
    if (!reader || !max_words || max_words > kAbsoluteWordLimit) {
        result.status = DeclarationStatus::InvalidConfiguration;
        return result;
    }
    if (!address) { result.status = DeclarationStatus::NullAddress; return result; }
    try {
        while (result.words.size() < max_words) {
            const std::size_t offset = result.words.size() * 4;
            if (offset > (std::numeric_limits<std::uintptr_t>::max)() - address ||
                address + offset > (std::numeric_limits<std::uintptr_t>::max)() - 3) {
                result.words.clear(); result.status = DeclarationStatus::AddressOverflow; return result;
            }
            unsigned char bytes[4]{};
            if (!Read(reader, context, address + offset, bytes, sizeof(bytes))) {
                result.words.clear(); result.status = DeclarationStatus::Unreadable; return result;
            }
            const std::uint32_t token = LittleEndian(bytes);
            result.words.push_back(token);
            if (token == kEnd) { result.status = DeclarationStatus::Complete; return result; }
            const unsigned type = token >> 29;
            std::size_t payload = 0;
            switch (type) {
            case 0: if (token != 0) { result.words.clear(); result.status = DeclarationStatus::UnsupportedToken; return result; } break;
            case 1: case 2: case 3: break; // stream, stream data, tessellator
            case 4: payload = ((token >> 25) & 0x0fu) * 4u; break; // CONSTMEM: count float4s
            case 5: payload = (token >> 24) & 0x1fu; break; // EXT: count DWORDs
            default: result.words.clear(); result.status = DeclarationStatus::UnsupportedToken; return result;
            }
            if (payload > max_words - result.words.size()) {
                result.words.clear(); result.status = DeclarationStatus::WordLimit; return result;
            }
            for (std::size_t i = 0; i < payload; ++i) {
                const std::size_t payload_offset = result.words.size() * 4;
                if (payload_offset > (std::numeric_limits<std::uintptr_t>::max)() - address ||
                    address + payload_offset > (std::numeric_limits<std::uintptr_t>::max)() - 3) {
                    result.words.clear(); result.status = DeclarationStatus::AddressOverflow; return result;
                }
                if (!Read(reader, context, address + payload_offset, bytes, sizeof(bytes))) {
                    result.words.clear(); result.status = DeclarationStatus::Unreadable; return result;
                }
                result.words.push_back(LittleEndian(bytes));
            }
        }
        result.words.clear(); result.status = DeclarationStatus::WordLimit;
    } catch (...) {
        result.words.clear(); result.status = DeclarationStatus::AllocationFailure;
    }
    return result;
}

} // namespace t3render::ir
