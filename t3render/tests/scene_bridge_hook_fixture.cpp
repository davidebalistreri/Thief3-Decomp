// Standalone x86 fixture: no game data, no game binary, no renderer.
#include "../src/scene_bridge_probe.hpp"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace t3render::scene_bridge;

extern "C" unsigned FixtureTarget();
extern "C" unsigned FixtureSubmission(void*);
extern "C" unsigned char FixtureSubmissionSite;

namespace {
unsigned started = 0, ended = 0;
unsigned live_samples = 0;
unsigned reentries = 0;
bool test_reentry = false, entered_recursively = false;
ProbeSnapshot last;
char events[16]{};
unsigned event_count = 0, submission_depth = 0;
bool nested_submission = false, nested_done = false, submission_args_ok = true;
void* submission_object = nullptr;

void SubmissionEnter(std::uint32_t element, std::uint32_t stage, std::uint32_t section) noexcept {
    submission_args_ok = submission_args_ok && element == reinterpret_cast<std::uint32_t>(submission_object) &&
                         stage == 2 && section == 5;
    if (event_count < sizeof(events)) events[event_count++] = 'E';
    ++submission_depth;
    __asm__ volatile("fninit\n\tpxor %%xmm0, %%xmm0\n\tpxor %%xmm7, %%xmm7" ::: "xmm0", "xmm7");
    SetLastError(123);
}
void SubmissionLeave() noexcept {
    submission_args_ok = submission_args_ok && submission_depth != 0;
    if (event_count < sizeof(events)) events[event_count++] = 'L';
    if (submission_depth) --submission_depth;
    __asm__ volatile("fninit\n\tpxor %%xmm0, %%xmm0\n\tpxor %%xmm7, %%xmm7" ::: "xmm0", "xmm7");
    SetLastError(456);
}
unsigned __attribute__((thiscall, noinline)) FakeDraw(void* object, unsigned stage, unsigned section) {
    submission_args_ok = submission_args_ok && object == submission_object && stage == 2 && section == 5 && submission_depth;
    if (event_count < sizeof(events)) events[event_count++] = 'D';
    if (nested_submission && !nested_done) {
        nested_done = true;
        submission_args_ok = submission_args_ok && FixtureSubmission(object) == 123;
    }
    return 123;
}

void Reentry() noexcept { ++reentries; }
bool SelectLive(std::uint64_t ticket) noexcept { ++live_samples; return ticket==1; }

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

// The five bytes at FixtureSubmissionSite reproduce only the callsite shape,
// not game code. The virtual callee consumes two thiscall stack arguments.
extern "C" __attribute__((naked, noinline)) unsigned FixtureSubmission(void*) {
    __asm__ volatile(
        "pushl %%ebx\n\tpushl %%esi\n\tpushl %%edi\n\t"
        "movl 16(%%esp), %%esi\n\tmovl $2, %%edi\n\tmovl $5, %%ebx\n\t"
        "movl (%%esi), %%edx\n\tpushl %%ebx\n\tpushl %%edi\n\t"
        ".globl _FixtureSubmissionSite\n_FixtureSubmissionSite:\n\t"
        ".byte 0x8b, 0xce\n\tcall *0x24(%%edx)\n\t"
        "popl %%edi\n\tpopl %%esi\n\tpopl %%ebx\n\tret" ::: "memory");
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
    if (argc == 2 && std::strcmp(argv[1], "--submission") == 0) {
        void* table[10]{};
        table[9] = reinterpret_cast<void*>(&FakeDraw);
        struct Object { void** table; } object{table};
        submission_object = &object;
        if (!InstallFixtureSubmissionProbe(&FixtureSubmissionSite, SubmissionEnter, SubmissionLeave, &status)) {
            std::printf("submission_install_failed=%u\n", static_cast<unsigned>(status)); return 1;
        }
        nested_submission = true;
        alignas(16) unsigned char before[512]{}, after[512]{};
        alignas(16) const std::uint32_t pattern0[4] = {1, 2, 3, 4}, pattern7[4] = {5, 6, 7, 8};
        unsigned result = 0;
        SetLastError(0x539);
        __asm__ volatile(
            "fninit\n\tfld1\n\tmovdqu %[p0], %%xmm0\n\tmovdqu %[p7], %%xmm7\n\t"
            "fxsave %[before]\n\tpushl %[object]\n\tcall _FixtureSubmission\n\taddl $4, %%esp\n\t"
            "movl %%eax, %[result]\n\tfxsave %[after]"
            : [before] "=m"(before), [after] "=m"(after), [result] "=m"(result)
            : [object] "r"(&object), [p0] "m"(pattern0), [p7] "m"(pattern7)
            : "eax", "ecx", "edx", "xmm0", "xmm7", "memory", "cc");
        const bool fp_ok = std::memcmp(before, after, 8) == 0 && std::memcmp(before + 24, after + 24, 4) == 0 &&
            std::memcmp(before + 32, after + 32, 10) == 0 && std::memcmp(before + 160, after + 160, 16) == 0 &&
            std::memcmp(before + 272, after + 272, 16) == 0;
        const bool valid = result == 123 && submission_args_ok && submission_depth == 0 &&
                           event_count == 6 && std::memcmp(events, "EDEDLL", 6) == 0 && GetLastError() == 0x539 && fp_ok;
        if (!DisableSubmissionProbe(&status)) return 1;
        const unsigned prior = event_count;
        const bool disabled = FixtureSubmission(&object) == 123 && event_count == prior + 1 && events[prior] == 'D';
        std::printf("submission_nested=%s fp=%s disable=%s\n", valid ? "PASS" : "FAIL", fp_ok ? "PASS" : "FAIL", disabled ? "PASS" : "FAIL");
        return valid && disabled ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--submission-bad-site") == 0) {
        const bool installed = InstallFixtureSubmissionProbe(reinterpret_cast<void*>(&FixtureTarget), SubmissionEnter,
                                                             SubmissionLeave, &status);
        const bool valid = !installed && status == ProbeStatus::PrologueMismatch && FixtureTarget() == 0x12345678;
        std::printf("submission_signature_rejected=%s\n", valid ? "PASS" : "FAIL");
        return valid ? 0 : 1;
    }
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
    const bool live_mode=argc==2 && std::strcmp(argv[1], "--live-sampling")==0;
    if (!InstallFixtureProbe(reinterpret_cast<void*>(&FixtureTarget), Begin, End, Reentry, &status,
                             live_mode ? SelectLive : nullptr)) {
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
    const bool live_ok=!live_mode || live_samples==2;
    std::printf("state=%s flags=%s call=%s gpr=%s disable=PASS live_sampling=%s\n", state_ok ? "PASS" : "FAIL",
                flags_ok ? "PASS" : "FAIL", call_ok ? "PASS" : "FAIL", gpr_ok ? "PASS" : "FAIL",
                live_ok ? "PASS" : "FAIL");
    return state_ok && flags_ok && call_ok && gpr_ok && live_ok ? 0 : 1;
}
