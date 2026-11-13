#pragma once

#include "vertex_declaration.hpp" // DeclarationReader is a fault-safe byte reader.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace t3render::ir {

// A framed application-memory observation, not proof of shader validity or of
// what the driver used. Only vs_1_1 is recognized; unsupported data fails
// closed and never exposes a partial program.
enum class ShaderProgramStatus {
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
    ShaderProgramStatus status = ShaderProgramStatus::Unreadable;
    std::vector<std::uint32_t> words;
};

// `reader` must return true only for a complete copy and must not reenter D3D.
// max_words includes the version and END tokens, and is limited to 4096.
ShaderProgramSnapshot ScanVertexShader11(std::uintptr_t address, DeclarationReader reader,
                                         void* context, std::size_t max_words) noexcept;

} // namespace t3render::ir
