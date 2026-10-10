#include "child_delivery.h"
#include "attachment.h"
#include "vendor/minhook/include/MinHook.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <mutex>

namespace fpp::agent {
namespace {
decltype(&CreateProcessW) originalCreate, originalBaseCreate;
decltype(&CreateProcessAsUserW) originalAsUser, originalBaseAsUser;
bool (*deliveryEnabled)() = nullptr;
std::wstring parentImage, adapterPath;
bool browser = false;
thread_local unsigned nesting = 0;
std::mutex diagnosticsMutex;
HANDLE diagnosticsMapping = nullptr;
ChildDeliveryDiagnostics *diagnostics = nullptr;
struct Guard {
    Guard() { ++nesting; }
    ~Guard() { --nesting; }
};
struct Arguments {
    LPWSTR *value = nullptr;
    ~Arguments() { if (value) LocalFree(value); }
};
std::wstring absolute(const wchar_t *path) {
    if (!path || !*path || PathIsRelativeW(path)) return {};
    wchar_t resolved[PathCapacity];
    DWORD n = GetFullPathNameW(path, (DWORD)PathCapacity, resolved, nullptr);
    return n && n < PathCapacity ? std::wstring(resolved, n) : std::wstring{};
}
bool eligible(const wchar_t *application, const wchar_t *command, DWORD flags) {
    if (nesting || !browser || !deliveryEnabled || !deliveryEnabled() || !command ||
        (flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | CREATE_PROTECTED_PROCESS))) return false;
    if (wcsnlen_s(command, PathCapacity) == PathCapacity) return false;
    int count = 0;
    Arguments arguments; arguments.value = CommandLineToArgvW(command, &count);
    if (!arguments.value || count < 2) return false;
    auto image = absolute(application ? application : arguments.value[0]);
    if (image.empty() || _wcsicmp(image.c_str(), parentImage.c_str())) return false;
    unsigned type = 0, subtype = 0, sandbox = 0;
    for (int i = 1; i < count; ++i) {
        auto arg = arguments.value[i];
        if (!wcsncmp(arg, L"--type=", 7)) {
            if (wcscmp(arg, L"--type=utility")) return false;
            ++type;
        } else if (!wcsncmp(arg, L"--utility-sub-type=", 19)) {
            if (wcscmp(arg, L"--utility-sub-type=chrome.mojom.UtilWin")) return false;
            ++subtype;
        } else if (!wcsncmp(arg, L"--service-sandbox-type=", 23)) {
            if (wcscmp(arg, L"--service-sandbox-type=none")) return false;
            ++sandbox;
        }
    }
    return type == 1 && subtype == 1 && sandbox == 1;
}
void record(const PROCESS_INFORMATION &process, DWORD flags, DeliveryStage stage,
            AttachmentResult result, DWORD resumeError) noexcept {
    try {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        if (!diagnostics) {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return;
            ULONGLONG stamp = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
            if (!diagnosticsMapping)
                diagnosticsMapping = createMapping(childDiagnosticName(GetCurrentProcessId(), stamp), sizeof(ChildDeliveryDiagnostics));
            if (!diagnosticsMapping) return;
            diagnostics = (ChildDeliveryDiagnostics *)MapViewOfFile(diagnosticsMapping, FILE_MAP_WRITE,
                                                                  0, 0, sizeof(ChildDeliveryDiagnostics));
            if (!diagnostics) return;
            diagnostics->version = AgentVersion; diagnostics->bytes = sizeof(*diagnostics);
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        ULONGLONG stamp = 0;
        if (GetProcessTimes(process.hProcess, &creation, &exit, &kernel, &user))
            stamp = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
        InterlockedIncrement(&diagnostics->sequence);
        diagnostics->state = result.state == ProcessState::Attached ? ChildDeliveryState::Installed : ChildDeliveryState::NativeFallback;
        diagnostics->parentPid = GetCurrentProcessId(); diagnostics->childPid = process.dwProcessId;
        diagnostics->childCreation = stamp; diagnostics->stage = (DWORD)stage;
        diagnostics->error = result.error; diagnostics->originalFlags = flags;
        diagnostics->resumeError = resumeError;
        MemoryBarrier(); InterlockedIncrement(&diagnostics->sequence);
    } catch (...) {}
}
struct ResumeChild {
    HANDLE thread;
    bool needed;
    DWORD error = 0;
    ResumeChild(HANDLE value, DWORD flags) : thread(value), needed(!(flags & CREATE_SUSPENDED)) {}
    void resume() noexcept {
        if (!needed) return;
        needed = false;
        if (ResumeThread(thread) == (DWORD)-1) {
            error = GetLastError();
            // A valid CreateProcess thread handle has resume rights. Retrying
            // through ntdll provides a final release attempt without terminating
            // the application if the public API unexpectedly fails.
            using NtResume = LONG (NTAPI *)(HANDLE, PULONG);
            auto fn = (NtResume)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeThread");
            if (fn && fn(thread, nullptr) >= 0) error = 0;
        }
    }
    ~ResumeChild() { resume(); }
};
void complete(const PROCESS_INFORMATION &process, DWORD flags) noexcept {
    // This guard exists before any allocating/throwing work after successful
    // creation, and removes only the suspension that this adapter introduced.
    ResumeChild release(process.hThread, flags);
    DeliveryStage stage = DeliveryStage::Identity;
    AttachmentResult result{ProcessState::Failed, ERROR_NOT_ENOUGH_MEMORY};
    try { result = attachSuspendedProcess(process.hProcess, process.dwProcessId, adapterPath, parentImage, stage); }
    catch (...) {}
    release.resume();
    record(process, flags, stage, result, release.error);
}
template<class F> BOOL launch(const wchar_t *application, wchar_t *command, DWORD flags,
                             PROCESS_INFORMATION *process, F call) noexcept {
    DWORD previousError = GetLastError();
    bool early = false;
    try { early = process && eligible(application, command, flags); } catch (...) {}
    Guard guard;
    SetLastError(previousError);
    BOOL result = call(early ? flags | CREATE_SUSPENDED : flags);
    DWORD nativeError = GetLastError();
    if (result && early) complete(*process, flags);
    SetLastError(nativeError);
    return result;
}
#define CREATE_ARGS LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES processSecurity, \
    LPSECURITY_ATTRIBUTES threadSecurity, BOOL inherit, DWORD flags, LPVOID environment, \
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process
#define CREATE_CALL application, command, processSecurity, threadSecurity, inherit, launchFlags, \
    environment, directory, startup, process
BOOL WINAPI create(CREATE_ARGS) {
    return launch(application, command, flags, process, [&](DWORD launchFlags) {
        return originalCreate(CREATE_CALL);
    });
}
BOOL WINAPI createBase(CREATE_ARGS) {
    return launch(application, command, flags, process, [&](DWORD launchFlags) {
        return originalBaseCreate(CREATE_CALL);
    });
}
BOOL WINAPI asUser(HANDLE token, CREATE_ARGS) {
    return launch(application, command, flags, process, [&](DWORD launchFlags) {
        return originalAsUser(token, CREATE_CALL);
    });
}
BOOL WINAPI asUserBase(HANDLE token, CREATE_ARGS) {
    return launch(application, command, flags, process, [&](DWORD launchFlags) {
        return originalBaseAsUser(token, CREATE_CALL);
    });
}
#undef CREATE_ARGS
#undef CREATE_CALL
bool hook(HMODULE module, const char *name, void *replacement, void **original) {
    auto target = module ? GetProcAddress(module, name) : nullptr;
    return target && MH_CreateHook((void *)target, replacement, original) == MH_OK;
}
bool alias(HMODULE base, HMODULE primary, const char *name, void *replacement, void **original) {
    auto target = base ? GetProcAddress(base, name) : nullptr;
    if (!target || target == GetProcAddress(primary, name)) return true;
    return hook(base, name, replacement, original);
}
} // namespace
bool installChildDeliveryHooks(bool (*enabled)()) {
    wchar_t image[PathCapacity]; DWORD n = (DWORD)PathCapacity;
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, image, &n)) return false;
    parentImage = absolute(image);
    auto leaf = PathFindFileNameW(parentImage.c_str());
    browser = !_wcsicmp(leaf, L"brave.exe") || !_wcsicmp(leaf, L"chrome.exe") ||
              !_wcsicmp(leaf, L"chromium.exe") || !_wcsicmp(leaf, L"msedge.exe");
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&installChildDeliveryHooks, &self)) return false;
    n = GetModuleFileNameW(self, image, (DWORD)PathCapacity);
    if (!n || n >= PathCapacity) return false;
    adapterPath.assign(image, n);
    deliveryEnabled = enabled;
    auto kernel = GetModuleHandleW(L"kernel32.dll"), base = GetModuleHandleW(L"kernelbase.dll");
    auto advapi = LoadLibraryW(L"advapi32.dll");
    return advapi && hook(kernel, "CreateProcessW", (void *)create, (void **)&originalCreate) &&
        alias(base, kernel, "CreateProcessW", (void *)createBase, (void **)&originalBaseCreate) &&
        hook(advapi, "CreateProcessAsUserW", (void *)asUser, (void **)&originalAsUser) &&
        alias(base, advapi, "CreateProcessAsUserW", (void *)asUserBase, (void **)&originalBaseAsUser);
}
} // namespace fpp::agent
