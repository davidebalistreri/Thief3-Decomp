#pragma once

#include "vertex_declaration.hpp" // DeclarationReader is a fault-safe byte reader.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace t3render::ir {

// A framed application-memory observation, not proof of shader validity or of
// what the driver used. VS 1.0 and 1.1 are framed; unsupported data fails
// closed and never exposes a partial program.
enum class ShaderProgramStatus {
    NotRequested,
    Complete,
    NullAddress,
    InvalidConfiguration,
    AddressOverflow,
    Unreadable,
    UnsupportedVersion,
    UnsupportedInstruction,
    InvalidParameter,
    WordLimit,
    AllocationFailure,
};

struct ShaderProgramSnapshot {
    ShaderProgramStatus status = ShaderProgramStatus::NotRequested;
    std::vector<std::uint32_t> words;
    bool first_word_known = false;
    std::uint32_t first_word = 0;
};

// `reader` must return true only for a complete copy and must not reenter D3D.
// max_words includes the version and END tokens, and is limited to 4096.
constexpr bool IsVertexShader1x(std::uint32_t word) noexcept {
    return word == 0xfffe0100u || word == 0xfffe0101u;
}
ShaderProgramSnapshot ScanVertexShader1x(std::uintptr_t address, DeclarationReader reader,
                                         void* context, std::size_t max_words) noexcept;

} // namespace t3render::ir
