#include "agent_shared.h"
#include "attachment.h"
#include "external_dialog.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <vector>
#include <algorithm>
#include <memory>

using namespace fpp::agent;
namespace {
constexpr wchar_t AgentClass[] = L"FPilotDialogAgent.v2";
constexpr UINT TrayMessage = WM_APP + 118, CommandMessage = WM_APP + 119;
enum Command : UINT { Show = 101, Enable, Pause, Startup, EditExclusions, Quit };
Policy *policy = nullptr;
HWND window = nullptr, list = nullptr, label = nullptr;
HANDLE hosts[2]{};
DWORD hostErrors[2]{}, hostRetries[2]{};
std::wstring folder, selectedRuntime, exclusionFile, configurationError;
UINT taskbarCreated = 0;
NOTIFYICONDATAW tray{};

std::wstring executable() { return folder + L"\\FPilotDialogAgent.exe"; }
DWORD stopExternalForInstall() {
    auto external = FindWindowW(fpp::external::WatcherClass, nullptr);
    if (!external) return ERROR_SUCCESS;
    DWORD pid = 0;
    GetWindowThreadProcessId(external, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError();
    // Installation switches only this package's controller. Its normal Exit
    // restores hidden native dialogs and stops private workers before returning.
    wchar_t image[PathCapacity]; DWORD length = PathCapacity;
    DWORD error = ERROR_SUCCESS;
    if (!QueryFullProcessImageNameW(process, 0, image, &length)) error = GetLastError();
    else if (_wcsicmp(image, (folder + L"\\FPilotDialogExternal.exe").c_str())) error = ERROR_BUSY;
    if (!error && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
        DWORD currentPid = 0; GetWindowThreadProcessId(external, &currentPid);
        if (currentPid != pid) error = ERROR_BUSY;
        else if (!PostMessageW(external, fpp::external::ControllerMessage, fpp::external::QuitCommand, 0)) error = GetLastError();
        else {
            auto wait = WaitForSingleObject(process, 10000);
            if (wait != WAIT_OBJECT_0) error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        }
    }
    CloseHandle(process);
    return error;
}
std::wstring runtimeFolder() {
    auto manifest = folder + L"\\agent-runtime.txt";
    HANDLE file = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char data[128]{}; DWORD bytes = 0;
    bool ok = ReadFile(file, data, sizeof(data) - 1, &bytes, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok || !bytes || bytes == sizeof(data) - 1) return {};
    std::string name(data, bytes);
    auto end = name.find_first_of("\r\n");
    if (end != std::string::npos) name.resize(end);
    if (name.rfind("AgentRuntime-2-", 0) != 0) return {};
    for (char c : name) if ((c < '0' || c > '9') && c != '-' &&
                            (c < 'A' || c > 'Z') && (c < 'a' || c > 'z')) return {};
    return folder + L"\\" + std::wstring(name.begin(), name.end());
}
bool readEnabled() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, RegistryPath, L"Enabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}
void persistEnabled(bool value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        DWORD enabled = value ? 1 : 0;
        RegSetValueExW(key, L"Enabled", 0, REG_DWORD, (BYTE *)&enabled, sizeof(enabled));
        RegCloseKey(key);
    }
}
std::wstring startupCommand() { return quote(executable()) + L" --background"; }
bool startsAtSignIn() {
    wchar_t value[PathCapacity + 32]; DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        StartupName, RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS && value == startupCommand();
}
DWORD setStartup(bool enable) {
    HKEY key = nullptr;
    DWORD error = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error) return error;
    if (enable) {
        auto command = startupCommand();
        error = RegSetValueExW(key, StartupName, 0, REG_SZ, (BYTE *)command.c_str(),
                               (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else if (startsAtSignIn()) {
        error = RegDeleteValueW(key, StartupName);
        if (error == ERROR_FILE_NOT_FOUND) error = 0;
    } else error = 0;
    RegCloseKey(key);
    return error;
}
void wakeHosts() {
    for (auto cls : {L"FPilotDialogHost64.v2", L"FPilotDialogHost32.v2"}) {
        HWND host = FindWindowW(cls, nullptr);
        if (host) PostMessageW(host, WakeMessage, 0, 0);
    }
}
void exclusions() {
    HANDLE file = CreateFileW(exclusionFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                              FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { configurationError = L"Cannot read exclusions."; return; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 65536 || size.QuadPart < 0) {
        CloseHandle(file); configurationError = L"Exclusions file is too large."; return;
    }
    std::vector<char> data((size_t)size.QuadPart);
    DWORD bytes = 0;
    bool read = ReadFile(file, data.data(), (DWORD)data.size(), &bytes, nullptr) != FALSE;
    CloseHandle(file);
    if (!read || bytes != data.size()) { configurationError = L"Cannot read exclusions."; return; }
    size_t begin = data.size() >= 3 && (BYTE)data[0] == 0xef && (BYTE)data[1] == 0xbb &&
                   (BYTE)data[2] == 0xbf ? 3 : 0;
    int count = data.empty() || begin == data.size() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        data.data() + begin, (int)(data.size() - begin), nullptr, 0);
    if ((count <= 0 && data.size() > begin) || count >= ExclusionCapacity) {
        configurationError = L"Exclusions must be UTF-8 and shorter than 8192 characters."; return;
    }
    std::wstring value((size_t)count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + begin,
                                  (int)(data.size() - begin), value.data(), count);
    if (value.find(L'\0') != std::wstring::npos) { configurationError = L"Invalid exclusions file."; return; }
    configurationError.clear();
    if (value == policy->exclusions) return;
    InterlockedIncrement(&policy->generation);
    wcsncpy_s(policy->exclusions, value.c_str(), _TRUNCATE);
    MemoryBarrier();
    InterlockedIncrement(&policy->generation);
    wakeHosts();
}
void initializeExclusions() {
    HANDLE file = CreateFileW(exclusionFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    constexpr char text[] = "# FilePilot dialog exclusions. UTF-8; one executable name or full path pattern per line.\r\n"
                            "# Examples: example.exe or C:\\Applications\\Example\\*\r\n"
                            "# Changes take effect automatically. Prefix comments with #.\r\n";
    DWORD bytes = 0; WriteFile(file, text, sizeof(text) - 1, &bytes, nullptr); CloseHandle(file);
}
void spawnHost(unsigned index) {
    if (hosts[index]) {
        if (WaitForSingleObject(hosts[index], 0) != WAIT_OBJECT_0) return;
        GetExitCodeProcess(hosts[index], &hostErrors[index]);
        CloseHandle(hosts[index]); hosts[index] = nullptr;
    }
    if (hostRetries[index] >= 3) return;
    ++hostRetries[index];
    auto path = selectedRuntime + (index ? L"\\FPilotDialogHost32.exe" : L"\\FPilotDialogHost64.exe");
    std::wstring command = quote(path);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, selectedRuntime.c_str(), &startup, &process)) {
        hostErrors[index] = GetLastError(); return;
    }
    CloseHandle(process.hThread); hosts[index] = process.hProcess; hostErrors[index] = 0;
}
std::vector<ProcessStatus> snapshot() {
    std::vector<ProcessStatus> result;
    for (auto suffix : {L"Status64", L"Status32"}) {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, objectName(suffix).c_str());
        if (!mapping) continue;
        auto view = (const HostStatus *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(HostStatus));
        if (view && view->version == AgentVersion) {
            for (unsigned attempt = 0; attempt != 3; ++attempt) {
                LONG sequence = view->sequence;
                if (sequence & 1) continue;
                MemoryBarrier();
                DWORD count = (std::min)(view->count, (DWORD)StatusCapacity);
                std::vector<ProcessStatus> copy(view->entries, view->entries + count);
                MemoryBarrier();
                if (sequence == view->sequence) { result.insert(result.end(), copy.begin(), copy.end()); break; }
            }
        }
        if (view) UnmapViewOfFile(view);
        CloseHandle(mapping);
    }
    std::sort(result.begin(), result.end(), [](auto &a, auto &b) { return _wcsicmp(a.path, b.path) < 0; });
    return result;
}
std::wstring lastRequest(const ProcessStatus &process) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, diagnosticName(process.pid, process.creation).c_str());
    if (!mapping) return L"";
    auto view = (const DialogDiagnostics *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(DialogDiagnostics));
    std::wstring result;
    if (view && view->version == AgentVersion && view->bytes == sizeof(DialogDiagnostics)) {
        for (unsigned attempt = 0; attempt != 3; ++attempt) {
            LONG sequence = view->sequence;
            if (sequence & 1) continue;
            MemoryBarrier();
            auto route = view->route; DWORD detail = view->detail;
            MemoryBarrier();
            if (sequence != view->sequence) continue;
            result = routeLabel(route);
            auto reason = route == DialogRoute::NativeConfiguration ? configurationLabel(detail) : nullptr;
            if (reason) result += std::wstring(L": ") + reason;
            else if (detail) { wchar_t text[32]; swprintf_s(text, L" (0x%08lX)", detail); result += text; }
            break;
        }
    }
    if (view) UnmapViewOfFile(view);
    CloseHandle(mapping);
    return result;
}
std::wstring lastChild(const ProcessStatus &process) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, childDiagnosticName(process.pid, process.creation).c_str());
    if (!mapping) return {};
    auto view = (const ChildDeliveryDiagnostics *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ChildDeliveryDiagnostics));
    std::wstring result;
    if (view && view->version == AgentVersion && view->bytes == sizeof(*view)) {
        for (unsigned attempt = 0; attempt != 3; ++attempt) {
            LONG sequence = view->sequence;
            if (sequence & 1) continue;
            MemoryBarrier();
            auto copy = *view;
            MemoryBarrier();
            if (sequence != view->sequence) continue;
            if (copy.state == ChildDeliveryState::None) break;
            result = L"Child " + std::to_wstring(copy.childPid) + (copy.state == ChildDeliveryState::Installed ?
                L": hooks installed before entry" : L": native fallback");
            if (copy.error) {
                const wchar_t *stage = L"attachment";
                switch ((DeliveryStage)copy.stage) {
                case DeliveryStage::Identity: stage = L"identity / serialization"; break;
                case DeliveryStage::Mitigations: stage = L"Windows restrictions"; break;
                case DeliveryStage::Loader: stage = L"loader initialization"; break;
                case DeliveryStage::Adapter: stage = L"adapter installation"; break;
                default: break;
                }
                result += L"; " + std::wstring(stage) + L" error " + std::to_wstring(copy.error);
            }
            if (copy.originalFlags & CREATE_SUSPENDED) result += L"; caller suspension retained";
            if (copy.resumeError) result += L"; resume error " + std::to_wstring(copy.resumeError);
            break;
        }
    }
    if (view) UnmapViewOfFile(view);
    CloseHandle(mapping);
    return result;
}
void refresh() {
    if (!list) return;
    auto rows = snapshot();
    unsigned attached = 0;
    int top = ListView_GetTopIndex(list);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    int row = 0;
    for (auto &item : rows) {
        item.path[_countof(item.path) - 1] = 0;
        LVITEMW entry{}; entry.mask = LVIF_TEXT; entry.iItem = row;
        entry.pszText = const_cast<LPWSTR>(PathFindFileNameW(item.path));
        ListView_InsertItem(list, &entry);
        auto pid = std::to_wstring(item.pid), error = item.error ? std::to_wstring(item.error) : L"";
        ListView_SetItemText(list, row, 1, pid.data());
        ListView_SetItemText(list, row, 2, const_cast<LPWSTR>(stateLabel(item.state)));
        ListView_SetItemText(list, row, 3, error.data());
        auto route = lastRequest(item);
        ListView_SetItemText(list, row, 4, route.data());
        auto child = lastChild(item);
        ListView_SetItemText(list, row, 5, child.data());
        ListView_SetItemText(list, row, 6, item.path);
        if (item.state == ProcessState::Attached) ++attached;
        ++row;
    }
    if (row) ListView_EnsureVisible(list, (std::min)(top, row - 1), FALSE);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
    std::wstring text = policy->enabled ? L"Enabled: " + std::to_wstring(attached) +
        L" applications attached. Supported Open / Save As requests use FilePilot." :
        L"Paused. New requests use Windows dialogs; active FilePilot dialogs can finish.";
    if (!configurationError.empty()) text += L" " + configurationError;
    for (unsigned i = 0; i != 2; ++i) if (hostErrors[i])
        text += (i ? L" x86 host error: " : L" x64 host error: ") + std::to_wstring(hostErrors[i]) + L".";
    SetWindowTextW(label, text.c_str());
    wcscpy_s(tray.szTip, policy->enabled ? L"FilePilot dialogs enabled" : L"FilePilot dialogs paused");
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}
void showWindow() { ShowWindow(window, SW_SHOWNORMAL); SetForegroundWindow(window); refresh(); }
void command(UINT id) {
    if (id == Show) showWindow();
    else if (id == Enable || id == Pause) {
        InterlockedExchange(&policy->enabled, id == Enable ? 1 : 0);
        persistEnabled(id == Enable); wakeHosts(); refresh();
    } else if (id == Startup) {
        DWORD error = setStartup(!startsAtSignIn());
        if (error) MessageBoxW(window, (L"Startup registration failed. Windows error " + std::to_wstring(error)).c_str(),
                               L"FilePilot dialogs", MB_OK | MB_ICONERROR);
    } else if (id == EditExclusions) {
        ShellExecuteW(window, L"open", exclusionFile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == Quit) DestroyWindow(window);
}
void popup() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, Show, L"Show application status");
    AppendMenuW(menu, MF_STRING, policy->enabled ? Pause : Enable, policy->enabled ? L"Pause replacement" : L"Enable replacement");
    AppendMenuW(menu, MF_STRING | (startsAtSignIn() ? MF_CHECKED : 0), Startup, L"Start at sign-in");
    AppendMenuW(menu, MF_STRING, EditExclusions, L"Edit application exclusions");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Quit, L"Exit");
    POINT cursor; GetCursorPos(&cursor); SetForegroundWindow(window);
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu); PostMessageW(window, WM_NULL, 0, 0);
    if (selected) command(selected);
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbarCreated && taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &tray); return 0; }
    if (message == CommandMessage || message == WM_COMMAND) { command(LOWORD(wparam)); return 0; }
    if (message == TrayMessage) {
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) popup();
        else if (lparam == WM_LBUTTONDBLCLK) showWindow();
        return 0;
    }
    if (message == WM_TIMER) {
        InterlockedExchange(&policy->heartbeat, (LONG)GetTickCount());
        exclusions();
        if ((GetTickCount() / 1000) % 5 == 0) { spawnHost(0); spawnHost(1); }
        if (IsWindowVisible(hwnd)) refresh();
        return 0;
    }
    if (message == WM_SIZE) {
        RECT rect; GetClientRect(hwnd, &rect);
        if (label) MoveWindow(label, 12, 12, (std::max)(0L, rect.right - 24), 46, TRUE);
        if (list) MoveWindow(list, 12, 64, (std::max)(0L, rect.right - 24), (std::max)(0L, rect.bottom - 76), TRUE);
        return 0;
    }
    if (message == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
    if (message == WM_QUERYENDSESSION) return TRUE;
    if (message == WM_ENDSESSION && wparam) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) {
        InterlockedExchange(&policy->enabled, 0); InterlockedExchange(&policy->ready, 0);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        for (auto cls : {L"FPilotDialogHost64.v2", L"FPilotDialogHost32.v2"}) {
            HWND host = FindWindowW(cls, nullptr); if (host) PostMessageW(host, WM_CLOSE, 0, 0);
        }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    folder = ownFolder();
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring action = argc > 1 ? argv[1] : L"";
    if (argv) LocalFree(argv);
    if (!action.empty() && action != L"--install" && action != L"--uninstall" &&
        action != L"--pause" && action != L"--enable" && action != L"--quit" &&
        action != L"--background" && action != L"--status") return ERROR_INVALID_PARAMETER;
    bool install = action == L"--install", uninstall = action == L"--uninstall";
    if (comModeInstalled() && !uninstall && action != L"--quit" && action != L"--pause") {
        if (action != L"--background") MessageBoxW(nullptr,
            L"FilePilot COM replacement is installed. Uninstall it before enabling the API agent.",
            L"FilePilot dialogs", MB_OK | MB_ICONINFORMATION);
        return ERROR_BUSY;
    }
    auto runtime = runtimeFolder();
    selectedRuntime = runtime;
    if (install && (runtime.empty() || GetFileAttributesW((runtime + L"\\FPilotPicker.exe").c_str()) == INVALID_FILE_ATTRIBUTES)) {
        MessageBoxW(nullptr, L"Keep the agent, hosts, bootstrap DLLs, runtime folder, and agent-runtime.txt together.",
                    L"FilePilot dialog package is incomplete", MB_OK | MB_ICONERROR); return ERROR_FILE_NOT_FOUND;
    }
    if (!uninstall && action != L"--quit" && action != L"--pause" && FindWindowW(fpp::external::WatcherClass, nullptr)) {
        DWORD error = install ? stopExternalForInstall() : ERROR_BUSY;
        if (error) {
            if (action != L"--background") MessageBoxW(nullptr,
                (install ? L"Could not stop this package's external interceptor. Windows error " + std::to_wstring(error) :
                 std::wstring(L"The external FilePilot interceptor is running. Use --install to switch this package to API interception.")).c_str(),
                L"FilePilot dialogs", MB_OK | MB_ICONINFORMATION);
            return (int)error;
        }
    }
    if (install || uninstall) {
        DWORD error = setStartup(install);
        if (error) {
            MessageBoxW(nullptr, (L"Startup registration failed. Windows error " + std::to_wstring(error)).c_str(),
                        L"FilePilot dialogs", MB_OK | MB_ICONERROR); return (int)error;
        }
        if (install) persistEnabled(true);
    }
    HWND existing = FindWindowW(AgentClass, nullptr);
    UINT requested = action == L"--pause" ? Pause : action == L"--enable" || install ? Enable :
                     action == L"--quit" || uninstall ? Quit : Show;
    if (action == L"--pause" || action == L"--enable") persistEnabled(requested == Enable);
    if (existing) {
        if (action != L"--background") PostMessageW(existing, CommandMessage, requested, 0);
        return 0;
    }
    if (requested == Quit || action == L"--pause") return 0;
    if (action == L"--status") {
        MessageBoxW(nullptr, L"The FilePilot dialog agent is not running.", L"FilePilot dialogs", MB_OK | MB_ICONINFORMATION);
        return ERROR_NOT_READY;
    }
    HANDLE singleton = CreateMutexW(nullptr, FALSE, objectName(L"Agent").c_str());
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return ERROR_ALREADY_EXISTS; }
    auto picker = runtime + L"\\FPilotPicker.exe";
    if (runtime.empty() || GetFileAttributesW(picker.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"Keep the agent, hosts, bootstrap DLLs, runtime folder, and agent-runtime.txt together.",
                    L"FilePilot dialog package is incomplete", MB_OK | MB_ICONERROR); CloseHandle(singleton); return ERROR_FILE_NOT_FOUND;
    }
    HANDLE mapping = createMapping(objectName(L"Policy"), sizeof(Policy));
    if (!mapping) { CloseHandle(singleton); return (int)GetLastError(); }
    policy = (Policy *)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Policy));
    if (!policy) { CloseHandle(mapping); CloseHandle(singleton); return (int)GetLastError(); }
    ZeroMemory(policy, sizeof(*policy));
    policy->magic = PolicyMagic; policy->version = AgentVersion; policy->bytes = sizeof(Policy);
    policy->pid = GetCurrentProcessId(); policy->enabled = readEnabled() ? 1 : 0;
    policy->heartbeat = (LONG)GetTickCount();
    wcscpy_s(policy->picker, picker.c_str());
    auto config = configurationFolder();
    if (config.empty()) { CloseHandle(mapping); CloseHandle(singleton); return ERROR_PATH_NOT_FOUND; }
    exclusionFile = config + L"\\exclusions.txt"; initializeExclusions(); exclusions();
    MemoryBarrier(); InterlockedExchange(&policy->ready, 1);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = AgentClass; cls.lpfnWndProc = windowProc;
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) return (int)GetLastError();
    window = CreateWindowExW(0, AgentClass, L"FilePilot dialog application status", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1050, 560, nullptr, nullptr, instance, nullptr);
    if (!window) return (int)GetLastError();
    label = CreateWindowExW(0, L"STATIC", L"Attaching applications...", WS_CHILD | WS_VISIBLE,
                            12, 12, 1000, 46, window, nullptr, instance, nullptr);
    list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT |
                           LVS_SINGLESEL, 12, 64, 1000, 410, window, nullptr, instance, nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t *titles[]{L"Application", L"PID", L"Attachment", L"Windows error", L"Last intercepted request", L"Last early child delivery", L"Executable"};
    int widths[]{180, 70, 290, 105, 360, 420, 500};
    for (int column = 0; column != 7; ++column) {
        LVCOLUMNW spec{}; spec.mask = LVCF_TEXT | LVCF_WIDTH; spec.cx = widths[column];
        spec.pszText = const_cast<LPWSTR>(titles[column]); ListView_InsertColumn(list, column, &spec);
    }
    auto font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE); SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
    HMENU menu = CreateMenu(), actions = CreatePopupMenu();
    for (auto item : {std::pair<UINT, const wchar_t *>{Enable, L"Enable replacement"}, {Pause, L"Pause replacement"},
                      {Startup, L"Toggle start at sign-in"}, {EditExclusions, L"Edit application exclusions"}, {Quit, L"Exit"}})
        AppendMenuW(actions, MF_STRING, item.first, item.second);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)actions, L"Actions"); SetMenu(window, menu);
    tray.cbSize = sizeof(tray); tray.hWnd = window; tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = TrayMessage;
    tray.hIcon = cls.hIcon; wcscpy_s(tray.szTip, L"FilePilot dialogs"); Shell_NotifyIconW(NIM_ADD, &tray);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    spawnHost(0); spawnHost(1); SetTimer(window, 1, 1000, nullptr);
    if (!install && action != L"--background") showWindow();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    for (auto host : hosts) if (host) CloseHandle(host);
    UnmapViewOfFile(policy); CloseHandle(mapping); CloseHandle(singleton);
    return 0;
}
