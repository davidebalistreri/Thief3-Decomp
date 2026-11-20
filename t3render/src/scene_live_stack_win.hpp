#pragma once

#include "scene_live_driver.hpp"
#include <windows.h>

namespace t3render::live {
// The marker is a live local in the observer's boundary frame, not a sampled
// stack pointer (which can move as outgoing call arguments are pushed). Only
// the 32-bit Windows path uses the downward-stack test. Missing evidence fails
// closed. The addresses are never persisted.
inline StackCursor CaptureStackCursor(const volatile void* marker) noexcept {
#if defined(__i386__) || defined(_M_IX86)
    const auto* tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    if (!marker || !tib || tib->Self!=tib) return {};
    const StackCursor value{reinterpret_cast<std::uintptr_t>(marker),
                            reinterpret_cast<std::uintptr_t>(tib->StackLimit),
                            reinterpret_cast<std::uintptr_t>(tib->StackBase)};
    return value.low && value.low<value.marker && value.marker<value.high ? value : StackCursor{};
#else
    (void)marker;return {};
#endif
}
} // namespace t3render::live
