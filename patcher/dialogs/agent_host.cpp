#include "attachment.h"
#include <tlhelp32.h>
#include <sddl.h>
#include <shlwapi.h>
#include <map>
#include <vector>
#include <algorithm>
#include <set>

using namespace fpp::agent;
namespace {
HANDLE stopEvent = nullptr, wakeEvent = nullptr;
Policy *policy = nullptr;
HostStatus *status = nullptr;
std::wstring currentSid, packageFolder;
DWORD currentSession = 0;
volatile LONG requestedPid = 0;
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Record {
    ULONGLONG creation = 0;
    ProcessStatus status{};
    DWORD attempts = 0, lastAttempt = 0;
    bool seen = false;
};
std::wstring patterns() {
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        LONG before = policy->generation;
        if (before & 1) continue;
        MemoryBarrier();
        std::wstring value(policy->exclusions, wcsnlen_s(policy->exclusions, ExclusionCapacity));
        MemoryBarrier();
        if (before == policy->generation) return value;
    }
    return L"*"; // A changing policy should not cause new attachments.
}
bool sameUser(HANDLE process, DWORD &integrity) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), bytes, &bytes)) return false;
    PWSTR text = nullptr;
    if (!ConvertSidToStringSidW(((TOKEN_USER *)buffer.data())->User.Sid, &text)) return false;
    bool match = currentSid == text;
    LocalFree(text);
    if (!match) return false;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &bytes);
    buffer.resize(bytes);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, buffer.data(), bytes, &bytes)) return false;
    auto sid = ((TOKEN_MANDATORY_LABEL *)buffer.data())->Label.Sid;
    integrity = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    return true;
}
bool matchingArchitecture(HANDLE process) {
    USHORT machine = 0, native = 0;
    if (!IsWow64Process2(process, &machine, &native)) return false;
    if (!machine) machine = native;
#ifdef _WIN64
    return machine == IMAGE_FILE_MACHINE_AMD64;
#else
    return machine == IMAGE_FILE_MACHINE_I386;
#endif
}
bool guiModules(DWORD pid) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{sizeof(entry)};
    if (Module32FirstW(snapshot.value, &entry)) do {
        if (!_wcsicmp(entry.szModule, L"user32.dll") || !_wcsicmp(entry.szModule, L"comdlg32.dll"))
            return true;
    } while (Module32NextW(snapshot.value, &entry));
    return false;
}
void publish(const std::map<DWORD, Record> &records) {
    InterlockedIncrement(&status->sequence);
    DWORD count = 0;
    for (auto &pair : records) {
        if (count == StatusCapacity) break;
        status->entries[count++] = pair.second.status;
    }
    status->count = count;
    MemoryBarrier();
    InterlockedIncrement(&status->sequence);
}
BOOL CALLBACK collectVisible(HWND window, LPARAM context) {
    if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid) ((std::set<DWORD> *)context)->insert(pid);
    }
    return TRUE;
}
DWORD WINAPI worker(void *) {
    std::map<DWORD, Record> records;
    std::wstring picker(policy->picker, wcsnlen_s(policy->picker, PathCapacity));
    auto runtimeFolder = picker.substr(0, picker.find_last_of(L"\\/"));
#ifdef _WIN64
    auto adapter = runtimeFolder + L"\\FPilotDialogs.dll";
#else
    auto adapter = runtimeFolder + L"\\FPilotDialogs32.dll";
#endif
    try {
        for (;;) {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 || !policyAlive(policy)) break;
            if (!policy->enabled) {
                HANDLE waits[]{stopEvent, wakeEvent};
                if (WaitForMultipleObjects(2, waits, FALSE, 1000) == WAIT_OBJECT_0) break;
                continue;
            }
            auto exclusions = patterns();
            for (auto &pair : records) pair.second.seen = false;
            Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            if (snapshot.value == INVALID_HANDLE_VALUE) {
                HANDLE waits[]{stopEvent, wakeEvent};
                if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) break;
                continue;
            }
            PROCESSENTRY32W entry{sizeof(entry)};
            std::vector<PROCESSENTRY32W> processes;
            if (Process32FirstW(snapshot.value, &entry)) do {
                processes.push_back(entry);
            } while (Process32NextW(snapshot.value, &entry));
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            DWORD requested = (DWORD)InterlockedExchange(&requestedPid, 0);
            std::set<DWORD> visible;
            EnumWindows(collectVisible, (LPARAM)&visible);
            auto priority = [&](DWORD pid) {
                if (pid == foreground) return 0;
                if (pid == requested) return 1;
                return visible.count(pid) ? 2 : 3;
            };
            // A slow background attachment must not delay the active browser
            // behind every other process during the initial reconciliation.
            std::stable_sort(processes.begin(), processes.end(), [&](auto &a, auto &b) {
                return priority(a.th32ProcessID) < priority(b.th32ProcessID);
            });
            for (auto &candidate : processes) {
                entry = candidate;
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
                DWORD pid = entry.th32ProcessID, session = 0;
                if (!pid || !ProcessIdToSessionId(pid, &session) || session != currentSession) continue;
                Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
                if (!process.value || !matchingArchitecture(process.value)) continue;
                DWORD integrity = 0;
                if (!sameUser(process.value, integrity) || integrity < SECURITY_MANDATORY_MEDIUM_RID) continue;
                wchar_t path[PathCapacity]; DWORD length = (DWORD)PathCapacity;
                if (!QueryFullProcessImageNameW(process.value, 0, path, &length)) continue;
                if (builtinExcluded(path)) continue;
                FILETIME creation{}, exit{}, kernel{}, user{};
                if (!GetProcessTimes(process.value, &creation, &exit, &kernel, &user)) continue;
                ULONGLONG stamp = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
                auto previous = records.find(pid);
                if (previous == records.end() && !guiModules(pid)) continue;
                auto &record = records[pid];
                if (record.creation != stamp) { record = {}; record.creation = stamp; }
                record.seen = true;
                record.status.pid = pid;
                record.status.creation = stamp;
                wcsncpy_s(record.status.path, path, _TRUNCATE);
                if (excluded(path, exclusions)) {
                    record.status.state = ProcessState::Excluded; record.status.error = 0;
                    continue;
                }
                if (record.status.state == ProcessState::Excluded) {
                    record.attempts = 0; record.lastAttempt = 0;
                }
                if (record.status.state == ProcessState::Attached ||
                    record.status.state == ProcessState::RestartRequired ||
                    record.status.state == ProcessState::Incompatible) continue;
                DWORD now = GetTickCount();
                if (record.attempts >= 3 || (record.attempts && now - record.lastAttempt < 30000)) continue;
                record.lastAttempt = now; ++record.attempts;
                PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
                PROCESS_MITIGATION_DYNAMIC_CODE_POLICY code{};
                if ((GetProcessMitigationPolicy(process.value, ProcessSignaturePolicy, &signature, sizeof(signature)) &&
                     (signature.MicrosoftSignedOnly || signature.StoreSignedOnly || signature.MitigationOptIn)) ||
                    (GetProcessMitigationPolicy(process.value, ProcessDynamicCodePolicy, &code, sizeof(code)) &&
                     code.ProhibitDynamicCode)) {
                    record.status.state = ProcessState::Incompatible;
                    record.status.error = ERROR_ACCESS_DISABLED_BY_POLICY;
                    continue;
                }
                Handle writable(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                    PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, pid));
                if (!writable.value) {
                    record.status.state = ProcessState::AccessDenied; record.status.error = GetLastError();
                    continue;
                }
                FILETIME actual{}, e{}, k{}, u{};
                if (!GetProcessTimes(writable.value, &actual, &e, &k, &u) ||
                    actual.dwHighDateTime != creation.dwHighDateTime || actual.dwLowDateTime != creation.dwLowDateTime)
                    continue;
                auto result = attachProcess(writable.value, pid, adapter);
                record.status.state = result.state; record.status.error = result.error;
                // Avoid repeated operations on a target whose remote initialization is still running.
                if (result.error == ERROR_TIMEOUT) record.attempts = 3;
                publish(records);
            }
            for (auto it = records.begin(); it != records.end();)
                if (!it->second.seen) it = records.erase(it); else ++it;
            publish(records);
            HANDLE waits[]{stopEvent, wakeEvent};
            if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) break;
        }
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
    return 0;
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WakeMessage) {
        if (wparam) InterlockedExchange(&requestedPid, (LONG)(DWORD)wparam);
        SetEvent(wakeEvent); return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { SetEvent(stopEvent); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE policyMapping = nullptr;
    policy = openPolicy(policyMapping);
    if (!policy || !policyAlive(policy)) return ERROR_NOT_READY;
    Handle singleton(CreateMutexW(nullptr, FALSE, objectName(statusSuffix()).append(L".Host").c_str()));
    if (!singleton.value || GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_ALREADY_EXISTS;
    currentSid = sid(); packageFolder = ownFolder();
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSession);
    Handle statusMapping(createMapping(objectName(statusSuffix()), sizeof(HostStatus)));
    if (!statusMapping.value) return (int)GetLastError();
    status = (HostStatus *)MapViewOfFile(statusMapping.value, FILE_MAP_WRITE, 0, 0, sizeof(HostStatus));
    if (!status) return (int)GetLastError();
    ZeroMemory(status, sizeof(*status));
    status->version = AgentVersion; status->pid = GetCurrentProcessId();
    status->machine = sizeof(void *) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle wake(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    stopEvent = stop.value; wakeEvent = wake.value;
    if (!stopEvent || !wakeEvent) return (int)GetLastError();
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = hostClass(); cls.lpfnWndProc = windowProc;
    if (!RegisterClassW(&cls)) return (int)GetLastError();
    HWND window = CreateWindowExW(0, hostClass(), L"FilePilot dialog attachment host", 0,
                                  0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return (int)GetLastError();
#ifdef _WIN64
    auto bootstrapPath = packageFolder + L"\\FPilotDialogBootstrap64.dll";
#else
    auto bootstrapPath = packageFolder + L"\\FPilotDialogBootstrap32.dll";
#endif
    HMODULE bootstrap = LoadLibraryW(bootstrapPath.c_str());
    auto hookProc = bootstrap ? (HOOKPROC)GetProcAddress(bootstrap, "FilePilotMessageHook") : nullptr;
    HHOOK hook = hookProc ? SetWindowsHookExW(WH_GETMESSAGE, hookProc, bootstrap, 0) : nullptr;
    // Reconciliation still works if Windows prevents global hook installation.
    Handle thread(CreateThread(nullptr, 0, worker, nullptr, 0, nullptr));
    if (!thread.value) { DestroyWindow(window); return (int)GetLastError(); }
    SetTimer(window, 1, 1000, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if ((message.message == WM_TIMER && !policyAlive(policy)) ||
            WaitForSingleObject(thread.value, 0) == WAIT_OBJECT_0) { DestroyWindow(window); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    SetEvent(stopEvent);
    if (hook) UnhookWindowsHookEx(hook);
    // Process teardown releases a worker still finishing a bounded remote operation.
    if (WaitForSingleObject(thread.value, 12000) != WAIT_OBJECT_0) ExitProcess(ERROR_TIMEOUT);
    // Do not unload a bootstrap while a cross-process callback may still be returning.
    UnmapViewOfFile(status); UnmapViewOfFile(policy); CloseHandle(policyMapping);
    return 0;
}
