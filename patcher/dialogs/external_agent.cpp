#include "external_dialog.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <map>
#include <memory>
#include <vector>

using namespace fpp::external;
namespace ag = fpp::agent;
namespace {
constexpr UINT CandidateMessage = WM_APP + 151, TrayMessage = WM_APP + 152, CommandMessage = ControllerMessage;
enum Command : UINT { Status = 201, Enable, Pause, Startup, Exclusions, Quit = QuitCommand };
HWND watcher = nullptr, list = nullptr, summary = nullptr;
bool enabled = true, closing = false;
std::wstring folder, runtime, picker, exclusionsPath, exclusionsText, configurationError;
NOTIFYICONDATAW tray{};
HWINEVENTHOOK objectHook = nullptr, foregroundHook = nullptr;
UINT taskbarCreated = 0;
unsigned long long nextJob = 0;
struct Job {
    HWND native = nullptr;
    DWORD pid = 0, tid = 0, exitCode = ERROR_IO_PENDING;
    ULONGLONG creation = 0;
    HANDLE process = nullptr, mapping = nullptr, helperProcess = nullptr;
    JobShared *shared = nullptr;
    std::wstring image, status;
    bool finished = false;
    ~Job() {
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        if (process) CloseHandle(process);
        if (helperProcess) CloseHandle(helperProcess);
    }
};
std::map<HWND, std::unique_ptr<Job>> jobs;
std::wstring executable() { return folder + L"\\FPilotDialogExternal.exe"; }
const wchar_t *startupName() {
    return ag::comModeInstalled() ? L"FilePilot Dialog Fallback" : ag::StartupName;
}
bool replacementEnabled() {
    if (!ag::comModeInstalled()) return true;
    DWORD value = 0, bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, ag::RegistryPath, L"ComEnabled",
        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value, &bytes) == ERROR_SUCCESS && value;
}
std::wstring runtimeFolder() {
    bool com = ag::comModeInstalled();
    HANDLE file = CreateFileW((folder + (com ? L"\\com-runtime.txt" : L"\\agent-runtime.txt")).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char data[128]{}; DWORD bytes = 0;
    bool read = ReadFile(file, data, sizeof(data) - 1, &bytes, nullptr) != FALSE;
    CloseHandle(file);
    if (!read || !bytes || bytes == sizeof(data) - 1) return {};
    std::string name(data, bytes);
    auto end = name.find_first_of("\r\n");
    if (end != name.npos) name.resize(end);
    if (name.rfind(com ? "ComRuntime-1-" : "AgentRuntime-2-", 0) != 0) return {};
    for (char c : name) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z') || c == '-')) return {};
    return folder + L"\\" + std::wstring(name.begin(), name.end());
}
void persistEnabled(bool value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ag::RegistryPath, 0, nullptr, 0, KEY_SET_VALUE,
                       nullptr, &key, nullptr) == ERROR_SUCCESS) {
        DWORD data = value ? 1 : 0;
        RegSetValueExW(key, L"ExternalEnabled", 0, REG_DWORD, (BYTE *)&data, sizeof(data));
        RegCloseKey(key);
    }
}
bool readEnabled() {
    DWORD data = 1, bytes = sizeof(data);
    RegGetValueW(HKEY_CURRENT_USER, ag::RegistryPath, L"ExternalEnabled", RRF_RT_REG_DWORD, nullptr, &data, &bytes);
    return data != 0;
}
std::wstring startupCommand() { return ag::quote(executable()) + L" --background"; }
bool startsAtSignIn() {
    wchar_t value[ag::PathCapacity + 32]; DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        startupName(), RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS && value == startupCommand();
}
DWORD setStartup(bool start) {
    HKEY key = nullptr;
    DWORD error = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error) return error;
    if (start) {
        wchar_t previous[ag::PathCapacity + 32]{}; DWORD bytes = sizeof(previous);
        DWORD type = 0;
        auto existing = RegQueryValueExW(key, startupName(), nullptr, &type, (BYTE *)previous, &bytes);
        auto previousAgent = ag::quote(folder + L"\\FPilotDialogAgent.exe") + L" --background";
        if (existing != ERROR_FILE_NOT_FOUND && (existing != ERROR_SUCCESS || type != REG_SZ ||
            bytes > sizeof(previous) || previous[(sizeof(previous) / sizeof(wchar_t)) - 1] != 0 ||
            (std::wstring(previous) != startupCommand() &&
             (ag::comModeInstalled() || std::wstring(previous) != previousAgent)))) {
            RegCloseKey(key); return ERROR_ALREADY_EXISTS;
        }
        auto text = startupCommand();
        error = RegSetValueExW(key, startupName(), 0, REG_SZ, (BYTE *)text.c_str(), (DWORD)((text.size() + 1) * sizeof(wchar_t)));
    } else if (startsAtSignIn()) {
        error = RegDeleteValueW(key, startupName());
        if (error == ERROR_FILE_NOT_FOUND) error = 0;
    }
    RegCloseKey(key);
    return error;
}
void readExclusions() {
    HANDLE file = CreateFileW(exclusionsPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { configurationError = L"Cannot read application exclusions."; return; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 65536) {
        CloseHandle(file); configurationError = L"Application exclusions file is too large."; return;
    }
    std::vector<char> bytes((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok || read != bytes.size()) { configurationError = L"Cannot read application exclusions."; return; }
    size_t begin = bytes.size() >= 3 && (BYTE)bytes[0] == 0xef && (BYTE)bytes[1] == 0xbb && (BYTE)bytes[2] == 0xbf ? 3 : 0;
    int count = begin == bytes.size() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data() + begin, (int)(bytes.size() - begin), nullptr, 0);
    if ((count <= 0 && begin < bytes.size()) || count >= ag::ExclusionCapacity) {
        configurationError = L"Application exclusions must be UTF-8 and shorter than 8192 characters."; return;
    }
    std::wstring value((size_t)count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + begin,
                                  (int)(bytes.size() - begin), value.data(), count);
    if (value.find(L'\0') != value.npos) { configurationError = L"Invalid application exclusions."; return; }
    exclusionsText = std::move(value); configurationError.clear();
}
void initializeExclusions() {
    HANDLE file = CreateFileW(exclusionsPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    constexpr char content[] = "# FilePilot application exclusions. UTF-8; executable names or full path patterns.\r\n";
    DWORD written = 0; WriteFile(file, content, sizeof(content) - 1, &written, nullptr); CloseHandle(file);
}
void restore(Job &job) {
    if (job.shared && InterlockedExchange(&job.shared->hidden, 0) && windowMatches(*job.shared))
        ShowWindowAsync(job.native, SW_SHOW);
}
void cancelWorker(Job &job) {
    if (!job.shared || job.finished) return;
    InterlockedExchange(&job.shared->stop, 1);
    auto remote = (HANDLE)(UINT_PTR)InterlockedCompareExchange64(&job.shared->cancelHandle, 0, 0);
    HANDLE cancellation = nullptr;
    // This duplicates an event from our own worker, never an application handle.
    if (remote && DuplicateHandle(job.process, remote, GetCurrentProcess(), &cancellation,
                                   EVENT_MODIFY_STATE, FALSE, 0)) {
        SetEvent(cancellation); CloseHandle(cancellation);
    }
    restore(job);
}
void rememberHelper(Job &job) {
    if (job.helperProcess || !job.shared->helperPid) return;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                 FALSE, (DWORD)job.shared->helperPid);
    if (!process) return;
    wchar_t image[ag::PathCapacity]; DWORD length = ag::PathCapacity;
    FILETIME start{}, exit{}, kernel{}, user{};
    ULONGLONG creation = 0;
    if (GetProcessTimes(process, &start, &exit, &kernel, &user))
        creation = (ULONGLONG(start.dwHighDateTime) << 32) | start.dwLowDateTime;
    if (creation && creation == (ULONGLONG)InterlockedCompareExchange64(&job.shared->helperCreation, 0, 0) &&
        QueryFullProcessImageNameW(process, 0, image, &length) && !_wcsicmp(image, picker.c_str())) job.helperProcess = process;
    else CloseHandle(process);
}
void reapHelper(Job &job) {
    // A recorded creation time and immutable picker path bind this handle to
    // the worker's private helper, even if its numeric PID is later reused.
    rememberHelper(job);
    if (job.helperProcess && WaitForSingleObject(job.helperProcess, 0) == WAIT_TIMEOUT)
        TerminateProcess(job.helperProcess, ERROR_CANCELLED);
}
void showStatus() {
    ShowWindow(watcher, SW_SHOWNORMAL); SetForegroundWindow(watcher);
}
void refresh() {
    if (!list) return;
    std::wstring headline = enabled && replacementEnabled() ?
        (ag::comModeInstalled() ? L"COM replacement with external fallback for native Open/Save windows." :
         L"External interception enabled. No FilePilot application DLL injection.") : L"External interception paused.";
    if (!configurationError.empty()) headline += L" " + configurationError;
    if (FindWindowW(L"FPilotDialogAgent.v2", nullptr)) headline += L" Injection agent is running; new external requests are suspended.";
    SetWindowTextW(summary, headline.c_str());
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    int row = 0;
    for (auto &entry : jobs) {
        auto &job = *entry.second;
        auto image = PathFindFileNameW(job.image.c_str());
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = const_cast<LPWSTR>(image);
        ListView_InsertItem(list, &item);
        auto pid = std::to_wstring(job.pid);
        ListView_SetItemText(list, row, 1, pid.data());
        ListView_SetItemText(list, row, 2, job.status.data());
        ListView_SetItemText(list, row, 3, job.image.data());
        ++row;
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
}
void command(UINT id) {
    if (id == Status) { refresh(); showStatus(); }
    else if (id == Enable || id == Pause) {
        enabled = id == Enable; persistEnabled(enabled);
        if (!enabled) for (auto &entry : jobs) cancelWorker(*entry.second);
        refresh();
    } else if (id == Startup) {
        DWORD error = setStartup(!startsAtSignIn());
        if (error) MessageBoxW(watcher, (L"Startup registration failed. Windows error " + std::to_wstring(error)).c_str(), L"FilePilot dialogs", MB_OK | MB_ICONERROR);
    } else if (id == Exclusions) ShellExecuteW(watcher, L"open", exclusionsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (id == Quit) DestroyWindow(watcher);
}
void popup() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, Status, L"Show external dialog status");
    AppendMenuW(menu, MF_STRING, enabled ? Pause : Enable, enabled ? L"Pause replacement" : L"Enable replacement");
    AppendMenuW(menu, MF_STRING | (startsAtSignIn() ? MF_CHECKED : 0), Startup, L"Start at sign-in");
    AppendMenuW(menu, MF_STRING, Exclusions, L"Edit application exclusions");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Quit, L"Exit");
    POINT cursor; GetCursorPos(&cursor); SetForegroundWindow(watcher);
    UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, watcher, nullptr);
    DestroyMenu(menu); PostMessageW(watcher, WM_NULL, 0, 0);
    if (action) command(action);
}
void candidate(HWND native) {
    if (closing || !enabled || !replacementEnabled() || !configurationError.empty() || FindWindowW(L"FPilotDialogAgent.v2", nullptr) ||
        !IsWindow(native) || !IsWindowVisible(native) || !IsWindowEnabled(native) || GetAncestor(native, GA_ROOT) != native) return;
    wchar_t cls[64]{}; GetClassNameW(native, cls, 64);
    if (wcscmp(cls, L"#32770") || GetPropW(native, ClaimProperty)) return;
    DWORD pid = 0; DWORD tid = GetWindowThreadProcessId(native, &pid);
    ULONGLONG creation = 0; std::wstring image;
    if (!eligibleProcess(pid, creation, image) || ag::excluded(image, exclusionsText)) return;
    auto old = jobs.find(native);
    if (old != jobs.end()) {
        if (!old->second->finished) return;
        jobs.erase(old);
    }
    unsigned active = 0;
    for (auto &entry : jobs) if (!entry.second->finished) ++active;
    if (active >= 8) return;
    while (jobs.size() >= 128) {
        auto done = std::find_if(jobs.begin(), jobs.end(), [](auto &entry) { return entry.second->finished; });
        if (done == jobs.end()) return;
        jobs.erase(done);
    }
    auto job = std::make_unique<Job>();
    job->native = native; job->pid = pid; job->tid = tid; job->creation = creation; job->image = image; job->status = L"Inspecting native dialog";
    auto mappingName = ag::objectName((L"ExternalJob." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++nextJob)).c_str());
    job->mapping = ag::createMapping(mappingName, sizeof(JobShared));
    if (!job->mapping) return;
    job->shared = (JobShared *)MapViewOfFile(job->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(JobShared));
    if (!job->shared) return;
    ZeroMemory(job->shared, sizeof(JobShared));
    job->shared->magic = JobMagic; job->shared->bytes = sizeof(JobShared);
    job->shared->controllerPid = GetCurrentProcessId(); job->shared->targetPid = pid; job->shared->targetThread = tid;
    job->shared->creation = creation; job->shared->window = (UINT64)(UINT_PTR)native;
    job->shared->heartbeat = (LONG)GetTickCount(); job->shared->phase = (LONG)Phase::Inspecting;
    wcscpy_s(job->shared->picker, picker.c_str());
    auto workerPath = runtime + L"\\FPilotDialogExternal.exe";
    auto arguments = ag::quote(workerPath) + L" --dialog " + ag::quote(mappingName);
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(workerPath.c_str(), arguments.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                         nullptr, runtime.c_str(), &startup, &process)) return;
    job->process = process.hProcess; job->shared->workerPid = process.dwProcessId;
    if (!SetPropW(native, ClaimProperty, (HANDLE)(UINT_PTR)process.dwProcessId)) {
        TerminateProcess(process.hProcess, ERROR_CANCELLED); CloseHandle(process.hThread); return;
    }
    MemoryBarrier();
    AllowSetForegroundWindow(process.dwProcessId);
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        RemovePropW(native, ClaimProperty);
        TerminateProcess(process.hProcess, ERROR_CANCELLED); CloseHandle(process.hThread); return;
    }
    CloseHandle(process.hThread);
    jobs.emplace(native, std::move(job));
}
void CALLBACK eventCallback(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (watcher && hwnd && (event == EVENT_SYSTEM_FOREGROUND || (object == OBJID_WINDOW && child == CHILDID_SELF)))
        PostMessageW(watcher, CandidateMessage, (WPARAM)hwnd, 0);
}
const wchar_t *phaseLabel(Phase phase) {
    switch (phase) {
    case Phase::Picker: return L"FilePilot picker active";
    case Phase::Submitting: return L"Submitting through native dialog";
    case Phase::Submitted: return L"Selection submitted; native validation retained";
    case Phase::Cancelled: return L"Cancellation forwarded";
    case Phase::Native: return L"Native fallback";
    case Phase::Stopped: return L"Stopped; native dialog restored";
    default: return L"Inspecting native dialog";
    }
}
std::wstring diagnosticStatus(const Job &job, const wchar_t *state) {
    std::wstring result = state;
    auto reason = (Reason)InterlockedCompareExchange(&job.shared->reason, 0, 0);
    if (reason != Reason::None) {
        result += L": "; result += reasonLabel(reason);
        auto error = (DWORD)InterlockedCompareExchange(&job.shared->error, 0, 0);
        if (error) {
            wchar_t code[24]{};
            swprintf_s(code, L" (0x%08lX)", error);
            result += code;
        }
    }
    return result;
}
void tick() {
    readExclusions();
    for (auto &entry : jobs) {
        auto &job = *entry.second;
        if (job.finished) continue;
        rememberHelper(job);
        if (WaitForSingleObject(job.process, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(job.process, &job.exitCode);
            reapHelper(job);
            restore(job); job.finished = true;
            job.status = job.exitCode ? diagnosticStatus(job, L"Native dialog retained") : phaseLabel((Phase)job.shared->phase);
        } else if ((DWORD)(GetTickCount() - (DWORD)job.shared->heartbeat) > 10000) {
            cancelWorker(job);
            // Only our isolated worker is terminated, never the requesting app.
            TerminateProcess(job.process, ERROR_TIMEOUT);
            reapHelper(job);
            job.status = diagnosticStatus(job, L"Worker timeout; native dialog restored");
            job.finished = true;
        } else {
            job.status = phaseLabel((Phase)job.shared->phase);
            if (!enabled || !replacementEnabled() || ag::excluded(job.image, exclusionsText)) cancelWorker(job);
        }
    }
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL { candidate(hwnd); return TRUE; }, 0);
    if (IsWindowVisible(watcher)) refresh();
}
void stopWorkers() {
    closing = true;
    for (auto &entry : jobs) cancelWorker(*entry.second);
    DWORD until = GetTickCount() + 2000;
    for (auto &entry : jobs) {
        auto &job = *entry.second;
        if (job.process && !job.finished) {
            DWORD left = (LONG)(until - GetTickCount()) > 0 ? until - GetTickCount() : 0;
            if (WaitForSingleObject(job.process, left) == WAIT_TIMEOUT) TerminateProcess(job.process, ERROR_CANCELLED);
            reapHelper(job);
        }
        restore(job);
        if (job.shared && windowMatches(*job.shared)) RemovePropW(job.native, ClaimProperty);
    }
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == taskbarCreated && taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &tray); return 0; }
    if (message == CandidateMessage) { candidate((HWND)w); return 0; }
    if (message == CommandMessage || message == WM_COMMAND) { command(LOWORD(w)); return 0; }
    if (message == TrayMessage) {
        if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) popup();
        else if (l == WM_LBUTTONDBLCLK) command(Status);
        return 0;
    }
    if (message == WM_TIMER) { tick(); return 0; }
    if (message == WM_SIZE) {
        RECT r; GetClientRect(hwnd, &r);
        if (summary) MoveWindow(summary, 12, 12, (std::max)(0L, r.right - 24), 44, TRUE);
        if (list) MoveWindow(list, 12, 64, (std::max)(0L, r.right - 24), (std::max)(0L, r.bottom - 76), TRUE);
        return 0;
    }
    if (message == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
    if (message == WM_QUERYENDSESSION) return TRUE;
    if (message == WM_ENDSESSION && w) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) {
        if (objectHook) UnhookWinEvent(objectHook);
        if (foregroundHook) UnhookWinEvent(foregroundHook);
        objectHook = foregroundHook = nullptr;
        stopWorkers(); Shell_NotifyIconW(NIM_DELETE, &tray); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring action = argc > 1 ? argv[1] : L"";
    std::wstring mapping = argc > 2 ? argv[2] : L"";
    if (argv) LocalFree(argv);
    if (action == L"--dialog") return mapping.empty() ? ERROR_INVALID_PARAMETER : runWorker(mapping);
    if (!action.empty() && action != L"--install" && action != L"--uninstall" && action != L"--background" &&
        action != L"--pause" && action != L"--enable" && action != L"--quit" && action != L"--status") return ERROR_INVALID_PARAMETER;
    folder = ag::ownFolder(); runtime = runtimeFolder(); picker = runtime + L"\\FPilotPicker.exe";
    bool install = action == L"--install", uninstall = action == L"--uninstall";
    if (install && (runtime.empty() || GetFileAttributesW(picker.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((runtime + L"\\FPilotDialogExternal.exe").c_str()) == INVALID_FILE_ATTRIBUTES)) return ERROR_FILE_NOT_FOUND;
    if (install || uninstall) {
        DWORD error = setStartup(install);
        if (error) return (int)error;
        if (install) persistEnabled(true);
    }
    UINT request = action == L"--pause" ? Pause : action == L"--enable" || install ? Enable :
        action == L"--quit" || uninstall ? Quit : Status;
    if (action == L"--pause" || action == L"--enable") persistEnabled(request == Enable);
    HWND existing = FindWindowW(WatcherClass, nullptr);
    if (existing) {
        if (action != L"--background") PostMessageW(existing, CommandMessage, request, 0);
        return 0;
    }
    if (request == Quit || action == L"--pause") return 0;
    if (action == L"--status") return ERROR_NOT_READY;
    if (runtime.empty() || GetFileAttributesW(picker.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((runtime + L"\\FPilotDialogExternal.exe").c_str()) == INVALID_FILE_ATTRIBUTES) return ERROR_FILE_NOT_FOUND;
    HANDLE singleton = CreateMutexW(nullptr, FALSE, ag::objectName(L"ExternalWatcher").c_str());
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return ERROR_ALREADY_EXISTS; }
    auto config = ag::configurationFolder();
    if (config.empty()) { CloseHandle(singleton); return ERROR_PATH_NOT_FOUND; }
    exclusionsPath = config + L"\\exclusions.txt"; initializeExclusions(); readExclusions(); enabled = readEnabled();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = WatcherClass; cls.lpfnWndProc = windowProc;
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    watcher = CreateWindowExW(0, WatcherClass, L"FilePilot external dialog interceptor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 480, nullptr, nullptr, instance, nullptr);
    if (!watcher) { CloseHandle(singleton); return (int)GetLastError(); }
    summary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 12, 940, 44, watcher, nullptr, instance, nullptr);
    list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        12, 64, 940, 340, watcher, nullptr, instance, nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t *titles[]{L"Application", L"PID", L"External dialog state", L"Executable"};
    int widths[]{170, 70, 470, 500};
    for (int i = 0; i != 4; ++i) {
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = widths[i]; column.pszText = const_cast<LPWSTR>(titles[i]);
        ListView_InsertColumn(list, i, &column);
    }
    auto font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(summary, WM_SETFONT, (WPARAM)font, TRUE); SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
    tray.cbSize = sizeof(tray); tray.hWnd = watcher; tray.uID = 1; tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = TrayMessage; tray.hIcon = cls.hIcon; wcscpy_s(tray.szTip, L"FilePilot external dialogs");
    Shell_NotifyIconW(NIM_ADD, &tray); taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    objectHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, eventCallback, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, eventCallback, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Reconciliation remains active if a desktop rejects an event subscription.
    SetTimer(watcher, 1, 500, nullptr); refresh();
    if (!install && action != L"--background") showStatus();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    jobs.clear(); CloseHandle(singleton); return 0;
}
