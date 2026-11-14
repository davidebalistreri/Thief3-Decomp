#pragma once

#include <cstdint>

namespace t3render::buffers {

enum class InvalidationCause { Internal, Explicit, StateBlock, ProcessVertices, ExternalPrivateData, UnlockMismatch };

// Observation-only classification; never authorizes a read or restores bytes.
enum class ShadowReadStatus {
    Captured, UnknownResource, UnknownData, Abandoned, BudgetExceeded,
    TooLarge, AllocationFailed, Busy, InvalidRange, Unavailable, Other
};

struct UnlockFailure {
    bool present = false;
    bool vertex = false;
    bool pending_lock = false;
    bool locked = false;
    bool pending_unlock = false;
    bool operation_exhausted = false;
    bool last_lock_known = false;
    std::int32_t last_lock_hr = 0;
    bool unlock_result_known = false;
    std::int32_t unlock_hr_candidate = 0;
};

struct DrawFailure {
    const char* status = "unavailable"; // static diagnostic string
    const char* cause = "unknown";      // static diagnostic string
    unsigned source_line = 0;           // buffer_proxy.cpp, 0 if unavailable
    UnlockFailure unlock{};
};

}  // namespace t3render::buffers
