#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace t3render::ir {

enum class DeclarationStatus {
    Complete,
    InvalidConfiguration,
    NullAddress,
    AddressOverflow,
    Unreadable,
    WordLimit,
    UnsupportedToken,
    AllocationFailure,
};

struct DeclarationSnapshot {
    DeclarationStatus status = DeclarationStatus::Unreadable;
    std::vector<std::uint32_t> words;
};

// The caller owns the read policy: true means exactly `size` bytes were copied.
// No pointer in the observed process is dereferenced by ScanDeclaration. The
// callback can use ReadProcessMemory; short reads must return false.
using DeclarationReader = bool (*)(void* context, std::uintptr_t address, void* out, std::size_t size);

// Frames a D3D8 declaration, including its END token, with bounded reads.
// CONST and EXT payloads are copied as opaque DWORDs; END in a payload is not
// a terminator. It does not validate driver semantics, decode a vertex layout,
// or prove that concurrently mutable bytes are what the driver consumed.
// Incomplete snapshots never expose partial words. `max_words` is inclusive
// of the END token and must be in [1, 4096].
DeclarationSnapshot ScanDeclaration(std::uintptr_t address, DeclarationReader reader, void* context,
                                    std::size_t max_words = 1024) noexcept;

} // namespace t3render::ir
