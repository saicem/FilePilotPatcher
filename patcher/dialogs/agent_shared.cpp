#include "agent_shared.h"
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <vector>

namespace fpp::agent {
bool comModeInstalled() {
    DWORD value = 0, bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, RegistryPath, L"ComModeInstalled",
        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value, &bytes) == ERROR_SUCCESS && value != 0;
}
std::wstring sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> data(bytes);
    bool ok = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    LPWSTR value = nullptr;
    if (!ok || !ConvertSidToStringSidW(((TOKEN_USER *)data.data())->User.Sid, &value)) return {};
    std::wstring result = value;
    LocalFree(value);
    return result;
}
std::wstring objectName(const wchar_t *suffix) {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    auto user = sid();
    if (user.empty()) return {};
    return L"Local\\FPilotDialogs.v2." + user + L"." + std::to_wstring(session) + L"." + suffix;
}
std::wstring ownFolder() {
    wchar_t path[PathCapacity];
    DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)PathCapacity);
    if (!n || n >= PathCapacity) return {};
    std::wstring value(path, n);
    return value.substr(0, value.find_last_of(L"\\/"));
}
std::wstring diagnosticName(DWORD pid, ULONGLONG creation) {
    return objectName((L"Dialog." + std::to_wstring(pid) + L"." + std::to_wstring(creation)).c_str());
}
std::wstring childDiagnosticName(DWORD pid, ULONGLONG creation) {
    return objectName((L"Children." + std::to_wstring(pid) + L"." + std::to_wstring(creation)).c_str());
}
std::wstring configurationFolder() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return {};
    std::wstring path = std::wstring(local) + L"\\Voidstar\\FilePilotDialogs";
    CoTaskMemFree(local);
    SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return path;
}
std::wstring quote(const std::wstring &value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'\"';
}
bool excluded(const std::wstring &path, const std::wstring &patterns) {
    auto leaf = PathFindFileNameW(path.c_str());
    size_t begin = 0;
    while (begin < patterns.size()) {
        auto end = patterns.find(L'\n', begin);
        auto pattern = patterns.substr(begin, end == std::wstring::npos ? end : end - begin);
        auto first = pattern.find_first_not_of(L" \t\r");
        auto last = pattern.find_last_not_of(L" \t\r");
        if (first != std::wstring::npos) {
            pattern = pattern.substr(first, last - first + 1);
            if (pattern[0] != L'#' && (PathMatchSpecW(path.c_str(), pattern.c_str()) ||
                                     PathMatchSpecW(leaf, pattern.c_str()))) return true;
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return false;
}
bool builtinExcluded(const std::wstring &path) {
    auto leaf = PathFindFileNameW(path.c_str());
    for (auto name : {L"FPilotPicker.exe", L"FPilotDialogAgent.exe", L"FPilotDialogExternal.exe", L"FPilotDialogHost64.exe",
                     L"FPilotDialogHost32.exe", L"FPilotDialogLauncher.exe",
                     L"FPilotDialogLauncher32.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
                     L"lsass.exe", L"services.exe", L"svchost.exe", L"smss.exe",
                     L"RuntimeBroker.exe", L"dllhost.exe", L"ApplicationFrameHost.exe",
                     L"sihost.exe", L"dwm.exe", L"audiodg.exe", L"fontdrvhost.exe",
                     L"SearchHost.exe", L"StartMenuExperienceHost.exe", L"ShellExperienceHost.exe"})
        if (!_wcsicmp(leaf, name)) return true;
    return false;
}
HANDLE createMapping(const std::wstring &name, DWORD bytes) {
    if (name.empty()) { SetLastError(ERROR_INVALID_NAME); return nullptr; }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto acl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid() + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                     bytes, name.c_str());
    DWORD error = GetLastError();
    LocalFree(descriptor);
    SetLastError(error);
    return mapping;
}
Policy *openPolicy(HANDLE &mapping, DWORD access) {
    mapping = OpenFileMappingW(access, FALSE, objectName(L"Policy").c_str());
    if (!mapping) return nullptr;
    auto policy = (Policy *)MapViewOfFile(mapping, access, 0, 0, sizeof(Policy));
    if (!policy || policy->magic != PolicyMagic || policy->version != AgentVersion ||
        policy->bytes != sizeof(Policy) || !policy->ready) {
        if (policy) UnmapViewOfFile(policy);
        CloseHandle(mapping); mapping = nullptr;
        return nullptr;
    }
    return policy;
}
bool policyAlive(const Policy *policy) {
    if (!policy || !policy->ready) return false;
    return (DWORD)(GetTickCount() - (DWORD)policy->heartbeat) < 15000;
}
const wchar_t *stateLabel(ProcessState state) {
    switch (state) {
    case ProcessState::Attached: return L"Adapter installed";
    case ProcessState::Excluded: return L"Excluded";
    case ProcessState::AccessDenied: return L"Access denied";
    case ProcessState::Incompatible: return L"Windows restriction / unsupported architecture";
    case ProcessState::RestartRequired: return L"Older adapter loaded; restart application";
    default: return L"Attachment failed";
    }
}
const wchar_t *routeLabel(DialogRoute route) {
    switch (route) {
    case DialogRoute::FilePilot: return L"FilePilot";
    case DialogRoute::NativeDisabled: return L"Windows: paused, excluded, or agent unavailable";
    case DialogRoute::NativeOptions: return L"Windows: unsupported options";
    case DialogRoute::NativeConfiguration: return L"Windows: customization / Shell configuration";
    case DialogRoute::NativeLegacy: return L"Windows: legacy configuration";
    case DialogRoute::NativeHelper: return L"Windows: picker unavailable";
    case DialogRoute::Failed: return L"Dialog request failed";
    case DialogRoute::Created: return L"Dialog created; Show not reached yet";
    case DialogRoute::FactoryCreated: return L"Dialog factory created; no dialog shown yet";
    case DialogRoute::NativeInterface: return L"Windows: unsupported COM interface (IID Data1)";
    case DialogRoute::NativeActivation: return L"Windows: aggregated / remote activation";
    default: return L"No intercepted request recorded";
    }
}
const wchar_t *configurationLabel(DWORD reason) {
    switch ((NativeConfiguration)reason) {
    case NativeConfiguration::CustomControls: return L"custom controls";
    case NativeConfiguration::SaveMetadata: return L"Save properties / metadata";
    case NativeConfiguration::VirtualFolder: return L"folder without a filesystem path";
    case NativeConfiguration::FileNameLabel: return L"filename label";
    case NativeConfiguration::Places: return L"custom Places";
    case NativeConfiguration::ShellFilter: return L"Shell item filter";
    case NativeConfiguration::CancelLabel: return L"cancel button label";
    case NativeConfiguration::NavigationRoot: return L"navigation root";
    default: return nullptr;
    }
}
} // namespace fpp::agent
