#include "agent_shared.h"
// The Windows hook only requests discovery. Loading libraries, installing API
// detours, and waiting for initialization belong to the attachment host worker.
extern "C" __declspec(dllexport) LRESULT CALLBACK FilePilotMessageHook(int code, WPARAM wparam, LPARAM lparam) {
    static volatile LONG requested = 0;
    if (code >= 0 && !requested) {
        HWND host = FindWindowW(fpp::agent::hostClass(), nullptr);
        DWORD pid = 0;
        if (host) GetWindowThreadProcessId(host, &pid);
        if (pid && pid != GetCurrentProcessId() &&
            PostMessageW(host, fpp::agent::WakeMessage, GetCurrentProcessId(), 0))
            InterlockedExchange(&requested, 1);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
