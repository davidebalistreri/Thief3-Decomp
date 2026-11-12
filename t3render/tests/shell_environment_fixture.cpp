// Isolated ShellExecuteExA inheritance probe. No game, proxy or graphics API
// is loaded. Three processes write their own environment snapshots; the first
// two wait for their child. It does not emulate Steam or Ion Launcher.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr DWORD kWaitMs = 15000;
constexpr const char* kKeys[] = {"T3RENDER_SHELL_MARKER", "T3RENDER_CAPTURE_PATH", "T3RENDER_CAPTURE_KEY",
                                  "T3RENDER_CAPTURE_BUFFERS", "T3RENDER_CAPTURE_FRAME"};

bool WriteEnvironment(const char* phase, const char* directory) {
    std::string output = std::string(directory) + "\\" + phase + ".env";
    HANDLE file = CreateFileA(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "cannot create %s (%lu)\n", output.c_str(), GetLastError()); return false; }
    std::string text;
    bool valid = true;
    for (const char* key : kKeys) {
        char value[4096]{};
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableA(key, value, sizeof(value));
        if (!n && GetLastError() != ERROR_ENVVAR_NOT_FOUND) { valid = false; break; }
        if (n >= sizeof(value) || std::strchr(value, '\n') || std::strchr(value, '\r')) { valid = false; break; }
        text += key; text += '='; text += n ? value : "<absent>"; text += '\n';
    }
    DWORD written = 0;
    const bool ok = valid && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    const bool closed = CloseHandle(file) != 0;
    if (!ok || !closed) { DeleteFileA(output.c_str()); return false; }
    return true;
}

bool StartAndWait(const char* child, const char* directory) {
    char exe[32768]{};
    const DWORD length = GetModuleFileNameA(nullptr, exe, sizeof(exe));
    if (!length || length >= sizeof(exe)) return false;
    std::string args = std::string(child) + " \"" + directory + "\"";
    SHELLEXECUTEINFOA info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = "open";
    info.lpFile = exe;
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExA(&info) || !info.hProcess) {
        std::fprintf(stderr, "ShellExecuteExA(%s) failed (%lu)\n", child, GetLastError()); return false;
    }
    const DWORD wait = WaitForSingleObject(info.hProcess, kWaitMs);
    DWORD status = 1;
    const bool finished = wait == WAIT_OBJECT_0 && GetExitCodeProcess(info.hProcess, &status) && status == 0;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(info.hProcess, 3); // Only the fixture child started above.
        WaitForSingleObject(info.hProcess, 2000);
    }
    CloseHandle(info.hProcess);
    if (!finished) std::fprintf(stderr, "child %s failed or timed out (wait=%lu status=%lu)\n", child, wait, status);
    return finished;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || (std::strcmp(argv[1], "parent") && std::strcmp(argv[1], "launcher") && std::strcmp(argv[1], "game")) ||
        !argv[2][0] || std::strchr(argv[2], '"') || std::strchr(argv[2], '\n') || std::strchr(argv[2], '\r')) {
        std::fprintf(stderr, "usage: shell_environment_fixture parent|launcher|game <existing-output-directory>\n"); return 2;
    }
    if (!WriteEnvironment(argv[1], argv[2])) return 1;
    if (!std::strcmp(argv[1], "parent") && !StartAndWait("launcher", argv[2])) return 1;
    if (!std::strcmp(argv[1], "launcher") && !StartAndWait("game", argv[2])) return 1;
    std::printf("shell_environment_%s=PASS\n", argv[1]);
    return 0;
}
