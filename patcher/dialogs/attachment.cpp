#include "attachment.h"
#include <tlhelp32.h>
#include <vector>
#include <stdexcept>
#include <psapi.h>
#include <sddl.h>

namespace fpp::agent {
namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct AttachmentLock {
    Handle mutex;
    bool acquired = false;
    AttachmentLock(HANDLE process, DWORD pid) {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) throw GetLastError();
        ULONGLONG stamp = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
        auto suffix = L"Attach." + std::to_wstring(pid) + L"." + std::to_wstring(stamp);
        mutex.value = CreateMutexW(nullptr, FALSE, objectName(suffix.c_str()).c_str());
        if (!mutex.value) throw GetLastError();
        DWORD wait = WaitForSingleObject(mutex.value, 5000);
        acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!acquired) throw (DWORD)(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    ~AttachmentLock() { if (acquired) ReleaseMutex(mutex.value); }
};
struct RemoteMemory {
    HANDLE process;
    void *address = nullptr;
    bool retain = false;
    RemoteMemory(HANDLE p, const void *data, SIZE_T bytes) : process(p) {
        address = VirtualAllocEx(p, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!address) throw GetLastError();
        if (!WriteProcessMemory(p, address, data, bytes, nullptr)) {
            DWORD error = GetLastError(); VirtualFreeEx(p, address, 0, MEM_RELEASE);
            address = nullptr; throw error;
        }
    }
    ~RemoteMemory() { if (address && !retain) VirtualFreeEx(process, address, 0, MEM_RELEASE); }
};
void read(HANDLE process, UINT_PTR address, void *out, SIZE_T bytes) {
    SIZE_T transferred = 0;
    if (!ReadProcessMemory(process, (void *)address, out, bytes, &transferred) || transferred != bytes)
        throw (DWORD)ERROR_PARTIAL_COPY;
}
template <class T> T read(HANDLE p, UINT_PTR address) {
    T value{}; read(p, address, &value, sizeof(value)); return value;
}
UINT_PTR module(DWORD pid, const std::wstring &identity, bool path) {
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (snapshot.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH) continue;
            throw GetLastError();
        }
        MODULEENTRY32W entry{sizeof(entry)};
        if (Module32FirstW(snapshot.value, &entry)) do {
            if (!_wcsicmp(path ? entry.szExePath : entry.szModule, identity.c_str()))
                return (UINT_PTR)entry.modBaseAddr;
        } while (Module32NextW(snapshot.value, &entry));
        return 0;
    }
    throw (DWORD)ERROR_BAD_LENGTH;
}
UINT_PTR remoteExport(HANDLE process, UINT_PTR base, const char *name) {
    auto dos = read<IMAGE_DOS_HEADER>(process, base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
        throw (DWORD)ERROR_BAD_EXE_FORMAT;
    auto nt = read<IMAGE_NT_HEADERS>(process, base + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE) throw (DWORD)ERROR_BAD_EXE_FORMAT;
    auto imageBytes = nt.OptionalHeader.SizeOfImage;
    auto bounded = [&](DWORD rva, SIZE_T bytes) {
        if (rva > imageBytes || bytes > imageBytes - rva) throw (DWORD)ERROR_BAD_EXE_FORMAT;
        return base + rva;
    };
    auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) return 0;
    auto exports = read<IMAGE_EXPORT_DIRECTORY>(process, bounded(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (exports.NumberOfNames > 65536 || exports.NumberOfFunctions > 65536)
        throw (DWORD)ERROR_BAD_EXE_FORMAT;
    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD rva = read<DWORD>(process, bounded(exports.AddressOfNames, exports.NumberOfNames * sizeof(DWORD)) + i * sizeof(DWORD));
        char text[256]{};
        bool terminated = false;
        for (DWORD c = 0; c < sizeof(text); ++c) {
            text[c] = read<char>(process, bounded(rva, c + 1) + c);
            if (!text[c]) { terminated = true; break; }
        }
        if (!terminated || strcmp(text, name)) continue;
        WORD ordinal = read<WORD>(process, bounded(exports.AddressOfNameOrdinals, exports.NumberOfNames * sizeof(WORD)) + i * sizeof(WORD));
        if (ordinal >= exports.NumberOfFunctions) throw (DWORD)ERROR_BAD_EXE_FORMAT;
        DWORD function = read<DWORD>(process, bounded(exports.AddressOfFunctions, exports.NumberOfFunctions * sizeof(DWORD)) + ordinal * sizeof(DWORD));
        if (function >= directory.VirtualAddress && function - directory.VirtualAddress < directory.Size)
            return 0; // This resolver is for the adapter's own, non-forwarded exports.
        return bounded(function, 1);
    }
    return 0;
}
DWORD invoke(HANDLE process, UINT_PTR address, RemoteMemory &parameter) {
    Handle thread(CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)address,
                                      parameter.address, 0, nullptr));
    if (!thread.value) throw GetLastError();
    DWORD wait = WaitForSingleObject(thread.value, 5000);
    if (wait != WAIT_OBJECT_0) {
        // A running remote thread may still dereference this allocation.
        parameter.retain = true;
        throw (DWORD)(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    DWORD result = 0;
    if (!GetExitCodeThread(thread.value, &result)) throw GetLastError();
    return result;
}
void initializeSuspendedLoader(HANDLE process) {
    // A newly created suspended child has no usable loader module list yet.
    // A secondary thread performs normal Windows loader initialization before
    // exiting, while the application's original thread stays suspended.
    auto local = GetModuleHandleW(L"ntdll.dll");
    auto exitThread = local ? GetProcAddress(local, "RtlExitUserThread") : nullptr;
    if (!exitThread) throw (DWORD)ERROR_PROC_NOT_FOUND;
    MEMORY_BASIC_INFORMATION region{};
    UINT_PTR cursor = 0, base = 0;
    wchar_t path[PathCapacity];
    while (VirtualQueryEx(process, (void *)cursor, &region, sizeof(region))) {
        if (region.Type == MEM_IMAGE && region.BaseAddress == region.AllocationBase &&
            GetMappedFileNameW(process, region.AllocationBase, path, (DWORD)PathCapacity)) {
            auto leaf = wcsrchr(path, L'\\');
            if (!_wcsicmp(leaf ? leaf + 1 : path, L"ntdll.dll")) {
                base = (UINT_PTR)region.AllocationBase; break;
            }
        }
        auto next = (UINT_PTR)region.BaseAddress + region.RegionSize;
        if (next <= cursor) break;
        cursor = next;
    }
    if (!base) throw (DWORD)ERROR_MOD_NOT_FOUND;
    Handle thread(CreateRemoteThread(process, nullptr, 0,
        (LPTHREAD_START_ROUTINE)(base + (UINT_PTR)exitThread - (UINT_PTR)local), nullptr, 0, nullptr));
    if (!thread.value) throw GetLastError();
    auto wait = WaitForSingleObject(thread.value, 5000);
    if (wait != WAIT_OBJECT_0) throw (DWORD)(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    DWORD result = 0;
    if (!GetExitCodeThread(thread.value, &result)) throw GetLastError();
    if (result) throw (DWORD)ERROR_DLL_INIT_FAILED;
}
bool eligibleIdentity(HANDLE process) {
    DWORD session = 0, ownSession = 0;
    if (!ProcessIdToSessionId(GetProcessId(process), &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) || session != ownSession)
        return false;
    USHORT machine = 0, native = 0;
    if (!IsWow64Process2(process, &machine, &native)) return false;
    if (!machine) machine = native;
#ifdef _WIN64
    if (machine != IMAGE_FILE_MACHINE_AMD64) return false;
#else
    if (machine != IMAGE_FILE_MACHINE_I386) return false;
#endif
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD appContainer = 0, bytes = sizeof(appContainer);
    if (!GetTokenInformation(token.value, TokenIsAppContainer, &appContainer, bytes, &bytes) || appContainer)
        return false;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> data(bytes);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), bytes, &bytes)) return false;
    auto ownSid = sid();
    PWSTR text = nullptr;
    if (!ConvertSidToStringSidW(((TOKEN_USER *)data.data())->User.Sid, &text)) return false;
    bool match = ownSid == text;
    LocalFree(text);
    if (!match) return false;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &bytes);
    data.resize(bytes);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, data.data(), bytes, &bytes)) return false;
    auto integrity = ((TOKEN_MANDATORY_LABEL *)data.data())->Label.Sid;
    return *GetSidSubAuthority(integrity, *GetSidSubAuthorityCount(integrity) - 1) >= SECURITY_MANDATORY_MEDIUM_RID;
}
UINT_PTR remoteLoadLibrary(DWORD pid) {
    auto address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE implementation = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &implementation)) throw GetLastError();
    wchar_t path[PathCapacity];
    DWORD n = GetModuleFileNameW(implementation, path, (DWORD)PathCapacity);
    if (!n || n >= PathCapacity) throw (DWORD)ERROR_BAD_PATHNAME;
    auto leaf = wcsrchr(path, L'\\');
    auto base = module(pid, leaf ? leaf + 1 : path, false);
    if (!base) throw (DWORD)ERROR_MOD_NOT_FOUND;
    return base + (UINT_PTR)address - (UINT_PTR)implementation;
}
} // namespace
AttachmentResult attachProcess(HANDLE process, DWORD pid, const std::wstring &adapter) {
    try {
        AttachmentLock lock(process, pid);
        auto base = module(pid, adapter, true);
        if (!base) {
            // Never stack this adapter over an older launcher or another package's adapter.
#ifdef _WIN64
            constexpr auto basename = L"FPilotDialogs.dll";
#else
            constexpr auto basename = L"FPilotDialogs32.dll";
#endif
            if (module(pid, basename, false)) return {ProcessState::RestartRequired, ERROR_REVISION_MISMATCH};
            RemoteMemory path(process, adapter.c_str(), (adapter.size() + 1) * sizeof(wchar_t));
            invoke(process, remoteLoadLibrary(pid), path);
            base = module(pid, adapter, true);
            if (!base) throw (DWORD)ERROR_DLL_INIT_FAILED;
        }
        auto initialize = remoteExport(process, base, "FilePilotAgentAttach");
        if (!initialize) return {ProcessState::RestartRequired, ERROR_REVISION_MISMATCH};
        AttachReply reply;
        RemoteMemory data(process, &reply, sizeof(reply));
        DWORD error = invoke(process, initialize, data);
        read(process, (UINT_PTR)data.address, &reply, sizeof(reply));
        if (error || reply.error) throw (DWORD)(error ? error : reply.error);
        constexpr DWORD required = HookCom | HookFactory | HookLegacy | HookComEx | HookChildren;
        if (reply.adapterVersion != AgentVersion || (reply.hooks & required) != required)
            return {ProcessState::RestartRequired, ERROR_REVISION_MISMATCH};
        return {ProcessState::Attached, ERROR_SUCCESS};
    } catch (DWORD error) {
        return {error == ERROR_ACCESS_DENIED ? ProcessState::AccessDenied : ProcessState::Failed, error};
    } catch (...) { return {ProcessState::Failed, ERROR_NOT_ENOUGH_MEMORY}; }
}
AttachmentResult attachSuspendedProcess(HANDLE process, DWORD pid, const std::wstring &adapter,
                                       const std::wstring &expectedImage, DeliveryStage &stage) {
    try {
        stage = DeliveryStage::Identity;
        if (!eligibleIdentity(process)) return {ProcessState::Incompatible, ERROR_ACCESS_DENIED};
        wchar_t image[PathCapacity]; DWORD length = (DWORD)PathCapacity;
        if (!QueryFullProcessImageNameW(process, 0, image, &length)) throw GetLastError();
        if (_wcsicmp(image, expectedImage.c_str()))
            return {ProcessState::Incompatible, ERROR_BAD_EXE_FORMAT};
        AttachmentLock lock(process, pid); // Serializes discovery and parent delivery.
        stage = DeliveryStage::Mitigations;
        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY code{};
        if (!GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature)) ||
            !GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &code, sizeof(code)))
            throw GetLastError();
        if (signature.MicrosoftSignedOnly || signature.StoreSignedOnly || signature.MitigationOptIn || code.ProhibitDynamicCode)
            return {ProcessState::Incompatible, ERROR_ACCESS_DISABLED_BY_POLICY};
        stage = DeliveryStage::Loader;
        initializeSuspendedLoader(process);
        stage = DeliveryStage::Adapter;
        return attachProcess(process, pid, adapter); // Windows mutexes permit this recursive acquisition.
    } catch (DWORD error) {
        return {error == ERROR_ACCESS_DENIED ? ProcessState::AccessDenied : ProcessState::Failed, error};
    } catch (...) { return {ProcessState::Failed, ERROR_NOT_ENOUGH_MEMORY}; }
}
} // namespace fpp::agent
