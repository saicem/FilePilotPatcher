#pragma once
#include "agent_shared.h"

namespace fpp::external {
constexpr DWORD JobMagic = 0x31455046;
constexpr wchar_t WatcherClass[] = L"FPilotDialogExternal.v1";
constexpr wchar_t ClaimProperty[] = L"FilePilot.ExternalDialog.v1";
constexpr UINT ControllerMessage = WM_APP + 153, QuitCommand = 206;
enum class Phase : LONG { Inspecting, Picker, Submitting, Submitted, Cancelled, Native, Stopped };
enum class Reason : LONG { None, WindowClass, Accessibility, AccessibilityTree, CustomOptions,
    FileView, Filename, FileType, Actions, ReadOnly, Mode, FilenamePattern, ActionPattern,
    Folder, Filters, Identity, WorkerWindow, Picker, Submission, Stopped, Exception };
const wchar_t *reasonLabel(Reason reason);
struct JobShared {
    DWORD magic, bytes, controllerPid, workerPid, targetPid, targetThread;
    ULONGLONG creation, window;
    volatile LONG heartbeat, stop, hidden, phase;
    alignas(8) volatile LONG64 cancelHandle;
    volatile LONG helperPid;
    alignas(8) volatile LONG64 helperCreation;
    volatile LONG reason, error;
    wchar_t picker[agent::PathCapacity];
};
bool windowMatches(const JobShared &job);
bool eligibleProcess(DWORD pid, ULONGLONG &creation, std::wstring &path);
int runWorker(const std::wstring &mappingName);
} // namespace fpp::external
