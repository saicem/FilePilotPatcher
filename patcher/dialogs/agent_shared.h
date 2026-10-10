#pragma once
#include <windows.h>
#include <string>

namespace fpp::agent {
constexpr DWORD PolicyMagic = 0x32415046, AgentVersion = 2;
constexpr size_t PathCapacity = 32768, ExclusionCapacity = 8192, StatusCapacity = 256;
constexpr UINT WakeMessage = WM_APP + 117;
constexpr wchar_t RegistryPath[] = L"Software\\Voidstar\\FilePilotDialogs";
constexpr wchar_t StartupName[] = L"FilePilot Dialog Agent";
struct Policy {
    DWORD magic, version, bytes, pid;
    volatile LONG ready, enabled, heartbeat, generation;
    wchar_t picker[PathCapacity];
    wchar_t exclusions[ExclusionCapacity];
};
enum class ProcessState : DWORD { Attached = 1, Excluded, AccessDenied, Incompatible,
                                  Failed, RestartRequired };
struct ProcessStatus {
    DWORD pid, error;
    ULONGLONG creation;
    ProcessState state;
    wchar_t path[512];
};
enum class DialogRoute : DWORD { None, FilePilot, NativeDisabled, NativeOptions,
                                NativeConfiguration, NativeLegacy, NativeHelper, Failed,
                                Created, FactoryCreated, NativeInterface, NativeActivation };
enum class NativeConfiguration : DWORD { CustomControls = 1, SaveMetadata, VirtualFolder,
                                        FileNameLabel, Places, ShellFilter, CancelLabel, NavigationRoot };
const wchar_t *configurationLabel(DWORD reason);
struct DialogDiagnostics {
    DWORD version, bytes;
    volatile LONG sequence;
    DialogRoute route;
    DWORD detail;
};
enum class ChildDeliveryState : DWORD { None, Installed, NativeFallback };
struct ChildDeliveryDiagnostics {
    DWORD version, bytes;
    volatile LONG sequence;
    ChildDeliveryState state;
    DWORD parentPid, childPid;
    ULONGLONG childCreation;
    DWORD stage, error, originalFlags, resumeError;
};
struct HostStatus {
    DWORD version, pid, machine;
    volatile LONG sequence;
    DWORD count;
    ProcessStatus entries[StatusCapacity];
};
struct AttachReply {
    DWORD bytes = sizeof(AttachReply), version = AgentVersion;
    DWORD adapterVersion = 0, hooks = 0, error = ERROR_IO_PENDING;
};
constexpr DWORD HookCom = 1, HookFactory = 2, HookLegacy = 4, HookComEx = 8, HookChildren = 16;
std::wstring sid();
std::wstring objectName(const wchar_t *suffix);
std::wstring diagnosticName(DWORD pid, ULONGLONG creation);
std::wstring childDiagnosticName(DWORD pid, ULONGLONG creation);
std::wstring ownFolder();
std::wstring configurationFolder();
std::wstring quote(const std::wstring &value);
bool excluded(const std::wstring &path, const std::wstring &patterns);
bool builtinExcluded(const std::wstring &path);
bool comModeInstalled();
HANDLE createMapping(const std::wstring &name, DWORD bytes);
Policy *openPolicy(HANDLE &mapping, DWORD access = FILE_MAP_READ);
bool policyAlive(const Policy *policy);
const wchar_t *stateLabel(ProcessState state);
const wchar_t *routeLabel(DialogRoute route);
inline const wchar_t *hostClass() {
#ifdef _WIN64
    return L"FPilotDialogHost64.v2";
#else
    return L"FPilotDialogHost32.v2";
#endif
}
inline const wchar_t *statusSuffix() {
#ifdef _WIN64
    return L"Status64";
#else
    return L"Status32";
#endif
}
} // namespace fpp::agent
