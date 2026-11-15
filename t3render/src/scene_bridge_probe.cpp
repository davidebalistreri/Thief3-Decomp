#include "scene_bridge_probe.hpp"
#include "scene_bridge_snapshot.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

#if !defined(_WIN32) || !defined(_M_IX86) && !defined(__i386__)
#error The scene bridge probe is restricted to Windows x86.
#endif

namespace t3render::scene_bridge {
namespace {

constexpr std::uintptr_t kImageBase = 0x10900000;
constexpr std::uintptr_t kTarget = 0x10926800;
constexpr DWORD kTimestamp = 0x40c8a4da;
constexpr DWORD kImageSize = 0x718000;
constexpr char kExecutableSha256[] = "09e0be902062c53cbb1736cffa1bd3ec883bd5e864f8e7f3f99bc0d06f6ad0f8";
constexpr char kPrologueSha256[] = "a4d662a480fee65cbb66de2d2cfa3aa83371a21e8c88e5ae7b78a9aca6e9ca07";
constexpr std::size_t kThunkSize = 128;

struct ProbeState {
    std::mutex mutex;
    std::atomic<BeginProbe> begin{nullptr};
    std::atomic<EndProbe> end{nullptr};
    std::atomic<MarkProbeReentry> reentry{nullptr};
    std::atomic<bool> active{false};
    bool attempted = false;
    void* target = nullptr;
    void* trampoline = nullptr;
    void* thunk = nullptr;
};

// The hook may still be on another thread's stack after DisableProbe returns.
// Neither the state nor executable thunk is ever freed during the process.
ProbeState* State() noexcept {
    static ProbeState* state = new (std::nothrow) ProbeState;
    return state;
}

bool SetStatus(ProbeStatus* status, ProbeStatus value, bool result = false) noexcept {
    if (status) *status = value;
    return result;
}

bool SafeRead(void*, std::uint32_t address, void* destination, std::size_t size) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
                             destination, size, &read) && read == size;
}

bool DigestEqual(const BYTE (&digest)[32], const char* expected) noexcept {
    constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 32; ++i) {
        if (expected[i * 2] != kHex[digest[i] >> 4] || expected[i * 2 + 1] != kHex[digest[i] & 15]) return false;
    }
    return expected[64] == '\0';
}

// Win32 CryptoAPI uses the same SHA-256 algorithm for the on-disk executable
// and the independently checked entry span. Never emit executable contents.
bool ComputeDigest(HANDLE file, const void* bytes, DWORD length, BYTE (&digest)[32]) noexcept {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool success = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        bool ok = true;
        if (file != INVALID_HANDLE_VALUE) {
            BYTE buffer[64 * 1024];
            DWORD count = 0;
            do {
                if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr) ||
                    (count && !CryptHashData(hash, buffer, count, 0))) { ok = false; break; }
            } while (count);
        } else ok = bytes && length && CryptHashData(hash, static_cast<const BYTE*>(bytes), length, 0);
        DWORD size = sizeof(digest);
        success = ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof(digest);
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return success;
}

bool VerifyExecutable(ProbeStatus* status) noexcept {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path) - 1) return SetStatus(status, ProbeStatus::ExecutableIdentityUnavailable);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SetStatus(status, ProbeStatus::ExecutableIdentityUnavailable);
    LARGE_INTEGER size{};
    BYTE digest[32]{};
    const bool valid_size = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 128 * 1024 * 1024;
    const bool hashed = valid_size && ComputeDigest(file, nullptr, 0, digest);
    CloseHandle(file);
    if (!hashed) return SetStatus(status, ProbeStatus::ExecutableIdentityUnavailable);
    if (!DigestEqual(digest, kExecutableSha256)) return SetStatus(status, ProbeStatus::ExecutableMismatch);
    return true;
}

bool VerifyImage(ProbeStatus* status) noexcept {
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase)
        return SetStatus(status, ProbeStatus::ImageMismatch);
    const ProbeReader reader{nullptr, SafeRead};
    IMAGE_DOS_HEADER dos{};
    if (!ReadProbe(reader, static_cast<std::uint32_t>(kImageBase), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024)
        return SetStatus(status, ProbeStatus::ImageMismatch);
    IMAGE_NT_HEADERS32 nt{};
    std::uint32_t address = 0;
    if (!ProbeOffset(static_cast<std::uint32_t>(kImageBase), static_cast<std::uint32_t>(dos.e_lfanew), address) ||
        !ReadProbe(reader, address, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.FileHeader.TimeDateStamp != kTimestamp ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt.OptionalHeader.ImageBase != kImageBase ||
        nt.OptionalHeader.SizeOfImage != kImageSize) return SetStatus(status, ProbeStatus::ImageMismatch);
    return true;
}

bool VerifyPrologue(ProbeStatus* status) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<void*>(kTarget), &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != kImageBase ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return SetStatus(status, ProbeStatus::PrologueMismatch);
    BYTE bytes[8]{}, digest[32]{};
    const ProbeReader reader{nullptr, SafeRead};
    if (!ReadProbe(reader, static_cast<std::uint32_t>(kTarget), bytes, sizeof(bytes)) ||
        !ComputeDigest(INVALID_HANDLE_VALUE, bytes, sizeof(bytes), digest) || !DigestEqual(digest, kPrologueSha256))
        return SetStatus(status, ProbeStatus::PrologueMismatch);
    return true;
}

// pushfd; pushad leaves (EDI, ESI, EBP, saved ESP, EBX, EDX, ECX, EAX,
// EFLAGS, return address). The thunk restores all of it after this function.
void __cdecl Dispatch(const std::uint32_t* registers) noexcept {
    const DWORD saved_error = GetLastError();
    thread_local bool entered = false;
    auto* state = State();
    if (state && registers && entered && state->active.load(std::memory_order_acquire)) {
        const auto reentry = state->reentry.load(std::memory_order_acquire);
        if (reentry) reentry();
    } else if (state && registers && !entered && state->active.load(std::memory_order_acquire)) {
        entered = true;
        const auto begin = state->begin.load(std::memory_order_acquire);
        const auto end = state->end.load(std::memory_order_acquire);
        if (begin && end) {
            const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
            const auto ticket = begin(thread);
            if (ticket) {
                const ProbeReader reader{nullptr, SafeRead};
                const ProbeSnapshot snapshot = SampleProbe(reader, thread, registers[6], registers[9]);
                end(ticket, snapshot);
            }
        }
        entered = false;
    }
    SetLastError(saved_error);
}

void Put32(BYTE*& output, std::uint32_t value) noexcept {
    std::memcpy(output, &value, sizeof(value));
    output += sizeof(value);
}

// The generated x86 wrapper saves flags/GPRs and the complete legacy
// FXSAVE state (x87, MMX, XMM and MXCSR) in a 16-byte-aligned stack buffer.
// It clears DF and initializes FP state for the C++ callback, then restores
// the game state before transferring to MinHook's relocated original code.
bool MakeThunk(ProbeState& state) noexcept {
    auto* memory = static_cast<BYTE*>(VirtualAlloc(nullptr, kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) return false;
    BYTE* p = memory;
    *p++ = 0x9c; *p++ = 0x60; *p++ = 0xfc;                        // pushfd; pushad; cld
    *p++ = 0x89; *p++ = 0xe3;                                      // mov ebx,esp (saved registers)
    *p++ = 0x81; *p++ = 0xec; Put32(p, 0x220);                     // sub esp,544
    *p++ = 0x83; *p++ = 0xe4; *p++ = 0xf0;                         // and esp,-16
    *p++ = 0x0f; *p++ = 0xae; *p++ = 0x04; *p++ = 0x24;            // fxsave [esp]
    *p++ = 0x89; *p++ = 0xe6;                                      // mov esi,esp
    *p++ = 0xdb; *p++ = 0xe3;                                      // fninit
    *p++ = 0x83; *p++ = 0xec; *p++ = 0x04;                         // sub esp,4
    *p++ = 0xc7; *p++ = 0x04; *p++ = 0x24; Put32(p, 0x1f80);       // mov [esp], default MXCSR
    *p++ = 0x0f; *p++ = 0xae; *p++ = 0x14; *p++ = 0x24;            // ldmxcsr [esp]
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04;                         // add esp,4
    *p++ = 0x83; *p++ = 0xec; *p++ = 0x0c; *p++ = 0x53;            // align stack; push ebx
    *p++ = 0xe8;
    Put32(p, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&Dispatch) -
                                       (reinterpret_cast<std::uintptr_t>(p) + 4)));
    *p++ = 0x89; *p++ = 0xf4;                                      // mov esp,esi
    *p++ = 0x0f; *p++ = 0xae; *p++ = 0x0c; *p++ = 0x24;            // fxrstor [esp]
    *p++ = 0x89; *p++ = 0xdc; *p++ = 0x61; *p++ = 0x9d;            // mov esp,ebx; popad; popfd
    *p++ = 0xff; *p++ = 0x25;
    Put32(p, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&state.trampoline))); // jmp [trampoline]
    if (static_cast<std::size_t>(p - memory) > kThunkSize) { VirtualFree(memory, 0, MEM_RELEASE); return false; }
    DWORD protection = 0;
    if (!VirtualProtect(memory, kThunkSize, PAGE_EXECUTE_READ, &protection) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, static_cast<SIZE_T>(p - memory))) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return false;
    }
    state.thunk = memory;
    return true;
}

bool InstallAt(ProbeState& state, void* target, BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status,
               bool verify_real_prologue) noexcept {
    if (!IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE))
        return SetStatus(status, ProbeStatus::UnsupportedProcessor);
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Dispatch), &pinned))
        return SetStatus(status, ProbeStatus::ModulePinFailed);
    if (!MakeThunk(state)) return SetStatus(status, ProbeStatus::HookEngineFailed);
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        VirtualFree(state.thunk, 0, MEM_RELEASE); state.thunk = nullptr;
        return SetStatus(status, ProbeStatus::HookEngineFailed);
    }
    state.target = target;
    if (MH_CreateHook(target, state.thunk, &state.trampoline) != MH_OK) {
        VirtualFree(state.thunk, 0, MEM_RELEASE); state.thunk = nullptr;
        return SetStatus(status, ProbeStatus::HookCreateFailed);
    }
    if (verify_real_prologue && !VerifyPrologue(status)) {
        MH_RemoveHook(target); VirtualFree(state.thunk, 0, MEM_RELEASE); state.thunk = nullptr;
        return false;
    }
    state.begin.store(begin, std::memory_order_release);
    state.end.store(end, std::memory_order_release);
    state.reentry.store(reentry, std::memory_order_release);
    state.active.store(true, std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK) {
        state.active.store(false, std::memory_order_release);
        MH_RemoveHook(target); VirtualFree(state.thunk, 0, MEM_RELEASE); state.thunk = nullptr;
        return SetStatus(status, ProbeStatus::HookEnableFailed);
    }
    return SetStatus(status, ProbeStatus::Installed, true);
}

}  // namespace

bool InstallProbe(BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status) noexcept {
    if (!begin || !end || !reentry) return SetStatus(status, ProbeStatus::InvalidArguments);
    auto* state = State();
    if (!state) return SetStatus(status, ProbeStatus::HookEngineFailed);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->attempted) return SetStatus(status, ProbeStatus::AlreadyAttempted);
    state->attempted = true;
    if (!VerifyExecutable(status) || !VerifyImage(status) || !VerifyPrologue(status)) return false;
    return InstallAt(*state, reinterpret_cast<void*>(kTarget), begin, end, reentry, status, true);
}

bool DisableProbe(ProbeStatus* status) noexcept {
    auto* state = State();
    if (!state) return SetStatus(status, ProbeStatus::HookEngineFailed);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active.load(std::memory_order_acquire)) return SetStatus(status, ProbeStatus::Disabled, true);
    if (MH_DisableHook(state->target) != MH_OK) return SetStatus(status, ProbeStatus::HookDisableFailed);
    state->active.store(false, std::memory_order_release);
    // A thread might still be returning from Dispatch or executing the
    // trampoline. Do not remove the hook, uninitialize MinHook or free memory.
    return SetStatus(status, ProbeStatus::Disabled, true);
}

#if defined(T3RENDER_SCENE_BRIDGE_TEST_FIXTURE)
// Compiled only into an isolated synthetic fixture, never the game proxy.
bool InstallFixtureProbe(void* target, BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status) noexcept {
    if (!target || !begin || !end || !reentry) return SetStatus(status, ProbeStatus::InvalidArguments);
    auto* state = State();
    if (!state) return SetStatus(status, ProbeStatus::HookEngineFailed);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->attempted) return SetStatus(status, ProbeStatus::AlreadyAttempted);
    state->attempted = true;
    return InstallAt(*state, target, begin, end, reentry, status, false);
}
#endif

}  // namespace t3render::scene_bridge
