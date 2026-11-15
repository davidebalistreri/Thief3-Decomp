// Standalone x86 fixture: no game data, no game binary, no renderer.
#include "../src/scene_bridge_probe.hpp"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace t3render::scene_bridge;

extern "C" unsigned FixtureTarget();

namespace {
unsigned started = 0, ended = 0;
unsigned reentries = 0;
bool test_reentry = false, entered_recursively = false;
ProbeSnapshot last;

void Reentry() noexcept { ++reentries; }

std::uint64_t Begin(std::uint32_t) noexcept {
    ++started;
    if (test_reentry && !entered_recursively) {
        entered_recursively = true;
        (void)FixtureTarget();
    }
    // Deliberately clobber the x87 stack and SIMD registers; the detour must
    // restore the interrupted game's state, not rely on the callback ABI.
    __asm__ volatile("fninit\n\tpxor %%xmm0, %%xmm0\n\tpxor %%xmm7, %%xmm7" ::: "xmm0", "xmm7");
    return 1;
}
void End(std::uint64_t ticket, const ProbeSnapshot& snapshot) noexcept {
    if (ticket == 1) { ++ended; last = snapshot; }
}
}

// An isolated target with an instruction boundary large enough for MinHook.
// The entry is synthetic; no instruction bytes are taken from the game.
extern "C" __attribute__((naked, noinline)) unsigned FixtureTarget() {
    __asm__ volatile("movl $0x12345678, %%eax\n\tret" ::: "eax");
}

static bool CheckGpr() {
    unsigned ok = 0;
    __asm__ volatile(
        "pushl %%ebx\n\tpushl %%esi\n\tpushl %%edi\n\tpushl %%ebp\n\t"
        "movl $0x13579bdf, %%ebx\n\tmovl $0x2468ace0, %%esi\n\t"
        "movl $0x11223344, %%edi\n\tmovl $0x55667788, %%ebp\n\t"
        "movl $0x10203040, %%edx\n\tcall _FixtureTarget\n\t"
        "cmpl $0x13579bdf, %%ebx\n\tjne 1f\n\t"
        "cmpl $0x2468ace0, %%esi\n\tjne 1f\n\t"
        "cmpl $0x11223344, %%edi\n\tjne 1f\n\t"
        "cmpl $0x55667788, %%ebp\n\tjne 1f\n\t"
        "cmpl $0x10203040, %%edx\n\tjne 1f\n\t"
        "movl $1, %%eax\n\tjmp 2f\n\t"
        "1: xorl %%eax, %%eax\n\t"
        "2: popl %%ebp\n\tpopl %%edi\n\tpopl %%esi\n\tpopl %%ebx\n\t"
        : "=a"(ok) : : "ecx", "edx", "cc", "memory");
    return ok == 1;
}

int main(int argc, char** argv) {
    ProbeStatus status{};
    if (argc == 2 && (std::strcmp(argv[1], "--read-perf") == 0 || std::strcmp(argv[1], "--read-perf-max") == 0)) {
        LARGE_INTEGER start{}, finish{}, frequency{};
        std::uint32_t source = 123, destination = 0;
        SIZE_T count = 0;
        const unsigned iterations = std::strcmp(argv[1], "--read-perf-max") == 0 ? 65536 : 6000;
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start)) return 1;
        for (unsigned i = 0; i < iterations; ++i)
            if (!ReadProcessMemory(GetCurrentProcess(), &source, &destination, sizeof(source), &count) || count != 4)
                return 1;
        if (!QueryPerformanceCounter(&finish) || destination != 123) return 1;
        const auto microseconds = (finish.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart;
        std::printf("read_count=%u elapsed_us=%lld\n", iterations, static_cast<long long>(microseconds));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--identity") == 0) {
        const bool installed = InstallProbe(Begin, End, Reentry, &status);
        const bool valid = !installed && status == ProbeStatus::ExecutableMismatch &&
                           FixtureTarget() == 0x12345678 && started == 0 && ended == 0;
        std::printf("identity_rejected=%s status=%u\n", valid ? "PASS" : "FAIL", static_cast<unsigned>(status));
        return valid ? 0 : 1;
    }
    if (!InstallFixtureProbe(reinterpret_cast<void*>(&FixtureTarget), Begin, End, Reentry, &status)) {
        std::printf("install_failed=%u\n", static_cast<unsigned>(status)); return 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--reentry") == 0) {
        test_reentry = true;
        SetLastError(0x539);
        const auto result = FixtureTarget();
        const bool valid = result == 0x12345678 && started == 1 && ended == 1 &&
                           reentries == 1 && GetLastError() == 0x539;
        std::printf("recursive_entry=%s\n", valid ? "PASS" : "FAIL");
        return valid ? 0 : 1;
    }
    alignas(16) unsigned char before[512]{};
    alignas(16) unsigned char after[512]{};
    alignas(16) const std::uint32_t pattern0[4] = {1, 2, 3, 4};
    alignas(16) const std::uint32_t pattern7[4] = {5, 6, 7, 8};
    unsigned before_flags = 0, after_flags = 0, result = 0, ecx = 0;
    SetLastError(0x539);
    // Preserve a live x87 value, XMM0/XMM7, MXCSR and control flags across the
    // callback. DF is cleared before returning to ordinary C++ code.
    __asm__ volatile(
        "fninit\n\t"
        "fld1\n\t"
        "movdqu %[p0], %%xmm0\n\t"
        "movdqu %[p7], %%xmm7\n\t"
        "fxsave %[before]\n\t"
        "movl $0x24681357, %%ecx\n\t"
        "stc\n\tstd\n\tpushfl\n\tpopl %[bf]\n\t"
        "call _FixtureTarget\n\t"
        "pushfl\n\tpopl %[af]\n\tcld\n\t"
        "movl %%eax, %[result]\n\t"
        "movl %%ecx, %[ecx]\n\t"
        "fxsave %[after]\n\t"
        : [before] "=m"(before), [after] "=m"(after), [bf] "=m"(before_flags),
          [af] "=m"(after_flags), [result] "=m"(result), [ecx] "=m"(ecx)
        : [p0] "m"(pattern0), [p7] "m"(pattern7)
        : "eax", "ecx", "edx", "xmm0", "xmm7", "memory", "cc");
    const DWORD saved_error = GetLastError();
    const bool state_ok = std::memcmp(before, after, 8) == 0 &&
        std::memcmp(before + 24, after + 24, 4) == 0 &&
        std::memcmp(before + 32, after + 32, 10) == 0 &&
        std::memcmp(before + 160, after + 160, 16) == 0 &&
        std::memcmp(before + 160 + 7 * 16, after + 160 + 7 * 16, 16) == 0;
    const bool flags_ok = ((before_flags ^ after_flags) & (1u | (1u << 10))) == 0;
    const bool call_ok = result == 0x12345678 && ecx == 0x24681357 && last.self == ecx &&
                         started == 1 && ended == 1 && saved_error == 0x539;
    const bool gpr_ok = CheckGpr() && started == 2 && ended == 2;
    if (!DisableProbe(&status)) { std::printf("disable_failed=%u\n", static_cast<unsigned>(status)); return 1; }
    if (FixtureTarget() != 0x12345678 || started != 2 || ended != 2) return 1;
    std::printf("state=%s flags=%s call=%s gpr=%s disable=PASS\n", state_ok ? "PASS" : "FAIL",
                flags_ok ? "PASS" : "FAIL", call_ok ? "PASS" : "FAIL", gpr_ok ? "PASS" : "FAIL");
    return state_ok && flags_ok && call_ok && gpr_ok ? 0 : 1;
}
