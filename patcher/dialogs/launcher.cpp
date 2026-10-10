#include "client.h"
#include <iostream>
#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

using namespace fpp;
namespace {
std::wstring ownFolder() {
    wchar_t path[32768];
    GetModuleFileNameW(nullptr, path, 32768);
    return parent(path);
}
UINT64 moduleBase(DWORD pid, const std::wstring &module) {
    for (int attempt = 0; attempt < 10; attempt++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH) {
                Sleep(10);
                continue;
            }
            return 0;
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        UINT64 base = 0;
        if (Module32FirstW(snap, &entry)) {
            do {
                if (!_wcsicmp(entry.szModule, leaf(module).c_str())) {
                    base = (UINT64)entry.modBaseAddr;
                    break;
                }
            } while (Module32NextW(snap, &entry));
        }
        CloseHandle(snap);
        return base;
    }
    return 0;
}
void *remoteSystem(DWORD pid, const wchar_t *dll, const char *function) {
    auto module = GetModuleHandleW(dll);
    auto address = GetProcAddress(module, function);
    HMODULE implementation = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)address, &implementation);
    wchar_t name[32768];
    GetModuleFileNameW(implementation, name, 32768);
    auto remote = moduleBase(pid, name);
    return remote ? (void *)(remote + (UINT64)address - (UINT64)implementation) : nullptr;
}
DWORD invoke(HANDLE process, void *address, void *parameter) {
    if (!address)
        throw std::runtime_error("remote function is unavailable");
    auto thread = CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)address,
                                     parameter, 0, nullptr);
    if (!thread)
        throw std::runtime_error("cannot create initialization thread");
    DWORD result = ERROR_TIMEOUT;
    auto wait = WaitForSingleObject(thread, 15000);
    if (wait == WAIT_OBJECT_0)
        GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error("adapter initialization timed out");
    return result;
}
void *copy(HANDLE process, const std::wstring &value) {
    auto bytes = (value.size() + 1) * 2;
    auto memory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory || !WriteProcessMemory(process, memory, value.c_str(), bytes, nullptr))
        throw std::runtime_error("cannot transfer adapter configuration");
    return memory;
}
void initializeLoader(HANDLE process) {
    // A process created suspended has not initialized its PEB loader list yet.
    // A short-lived secondary thread initializes imports while its application
    // entry thread remains suspended. Locate ntdll from mapped memory, not an
    // assumed system-DLL address or the still-uninitialized module snapshot.
    auto local = GetModuleHandleW(L"ntdll.dll");
    auto exitThread = GetProcAddress(local, "RtlExitUserThread");
    MEMORY_BASIC_INFORMATION region{};
    UINT64 address = 0, base = 0;
    wchar_t name[32768];
    while (VirtualQueryEx(process, (void *)(ULONG_PTR)address, &region, sizeof(region))) {
        if (region.Type == MEM_IMAGE &&
            GetMappedFileNameW(process, region.AllocationBase, name, 32768) &&
            !_wcsicmp(leaf(name).c_str(), L"ntdll.dll")) {
            base = (UINT64)region.AllocationBase;
            break;
        }
        auto next = (UINT64)region.BaseAddress + region.RegionSize;
        if (next <= address)
            break;
        address = next;
    }
    if (!base || !exitThread)
        throw std::runtime_error("cannot locate the application loader");
    invoke(process, (void *)(ULONG_PTR)(base + (UINT64)exitThread - (UINT64)local), nullptr);
}
WORD machine(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open application executable");
    IMAGE_DOS_HEADER dos{};
    DWORD n;
    bool ok = ReadFile(file, &dos, sizeof(dos), &n, nullptr) && n == sizeof(dos) &&
              dos.e_magic == IMAGE_DOS_SIGNATURE;
    if (!ok) {
        CloseHandle(file);
        throw std::runtime_error("invalid application executable");
    }
    SetFilePointer(file, dos.e_lfanew + 4, nullptr, FILE_BEGIN);
    WORD value = 0;
    ReadFile(file, &value, 2, &n, nullptr);
    CloseHandle(file);
    return value;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    PROCESS_INFORMATION process{};
    try {
        auto dir = ownFolder();
        std::wstring requested;
        if (argc < 2) {
            auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            Request request;
            request.title = L"Choose an application — FilePilot";
            request.okLabel = L"Launch";
            request.options |= FOS_FORCEFILESYSTEM | FOS_STRICTFILETYPES;
            request.filters = {{L"Windows applications", L"*.exe"}};
            PWSTR programs = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programs))) {
                request.folder = programs;
                CoTaskMemFree(programs);
            } else {
                request.folder = dir;
            }
            Result selection;
            auto result = runPicker(
                dir + L"\\FPilotPicker.exe", request, nullptr,
                [](Event, Result &, std::wstring &) { return S_OK; }, selection);
            if (SUCCEEDED(apartment))
                CoUninitialize();
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                return 0;
            if (FAILED(result))
                throw std::runtime_error("the application chooser could not start");
            requested = selection.paths.at(0);
        } else {
            requested = argv[1];
        }
        wchar_t found[32768];
        DWORD n = SearchPathW(nullptr, requested.c_str(), L".exe", 32768, found, nullptr);
        if (!n || n >= 32768)
            throw std::runtime_error("application executable was not found; run the launcher "
                                     "without arguments to choose an application");
        std::wstring application = found;
        auto target = machine(application);
#ifdef _WIN64
        if (target == IMAGE_FILE_MACHINE_I386) {
            auto launcher = dir + L"\\FPilotDialogLauncher32.exe";
            if (GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES)
                throw std::runtime_error(
                    "x86 support is not installed; rebuild with the x86 package");
            std::wstring cmd = quote(launcher) + L" " + quote(application);
            for (int i = 2; i < argc; i++)
                cmd += L" " + quote(argv[i]);
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(launcher.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                nullptr, &startup, &child))
                throw std::runtime_error("cannot launch x86 adapter loader");
            auto wait = WaitForSingleObject(child.hProcess, 60000);
            DWORD exitCode = ERROR_TIMEOUT;
            if (wait == WAIT_OBJECT_0)
                GetExitCodeProcess(child.hProcess, &exitCode);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            return (int)exitCode;
        }
        if (target != IMAGE_FILE_MACHINE_AMD64)
            throw std::runtime_error("this package supports x64 and x86 Windows applications");
        auto adapter = dir + L"\\FPilotDialogs.dll";
#else
        if (target != IMAGE_FILE_MACHINE_I386)
            throw std::runtime_error("use the x64 launcher for this application");
        auto adapter = dir + L"\\FPilotDialogs32.dll";
#endif
        auto helper = dir + L"\\FPilotPicker.exe";
        if (GetFileAttributesW(adapter.c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error(
                "the launcher, adapter DLL, and picker must be in the same package directory");
        std::wstring command = quote(application);
        for (int i = 2; i < argc; i++)
            command += L" " + quote(argv[i]);
        STARTUPINFOW startup{sizeof(startup)};
        if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
            throw std::runtime_error("cannot start application");
        initializeLoader(process.hProcess);
        auto remotePath = copy(process.hProcess, adapter);
        invoke(process.hProcess, remoteSystem(process.dwProcessId, L"kernel32.dll", "LoadLibraryW"),
               remotePath);
        VirtualFreeEx(process.hProcess, remotePath, 0, MEM_RELEASE);
        auto base = moduleBase(process.dwProcessId, adapter);
        if (!base)
            throw std::runtime_error("Windows could not load the adapter into this application");
        auto local = LoadLibraryExW(adapter.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!local)
            throw std::runtime_error("cannot inspect adapter exports");
        auto initialize = GetProcAddress(local, "FilePilotInstall");
        if (!initialize) {
            FreeLibrary(local);
            throw std::runtime_error("adapter initialization export is missing");
        }
        auto rva = (UINT64)initialize - (UINT64)local;
        FreeLibrary(local);
        auto remoteHelper = copy(process.hProcess, helper);
        auto error = invoke(process.hProcess, (void *)(base + rva), remoteHelper);
        VirtualFreeEx(process.hProcess, remoteHelper, 0, MEM_RELEASE);
        if (error)
            throw std::runtime_error("adapter could not install its dialog hooks");
        if (ResumeThread(process.hThread) == (DWORD)-1)
            throw std::runtime_error("cannot resume application");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        process = {};
        std::wcout << L"FilePilot dialogs enabled for " << application << L"\n";
        return 0;
    } catch (const std::exception &e) {
        if (process.hProcess) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        std::cerr << "FilePilot dialog launcher: " << e.what() << " (Windows error "
                  << GetLastError() << ")\n";
        return 1;
    }
}
