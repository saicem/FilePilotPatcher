#include "client.h"
#include <cwctype>
#include <memory>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace fpp {
class BindData final : public IFileSystemBindData {
    LONG refs = 1;
    WIN32_FIND_DATAW value{};

  public:
    explicit BindData(const std::wstring &name) {
        value.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        wcsncpy_s(value.cFileName, name.c_str(), _TRUNCATE);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IFileSystemBindData)
            return E_NOINTERFACE;
        *out = (IFileSystemBindData *)this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = InterlockedDecrement(&refs);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE SetFindData(const WIN32_FIND_DATAW *data) override {
        if (!data)
            return E_POINTER;
        value = *data;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFindData(WIN32_FIND_DATAW *data) override {
        if (!data)
            return E_POINTER;
        *data = value;
        return S_OK;
    }
};
HRESULT shellItem(const std::wstring &path, IShellItem **out) {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(out));
    ComPtr<IBindCtx> context;
    HRESULT hr = CreateBindCtx(0, &context);
    if (FAILED(hr))
        return hr;
    ComPtr<IFileSystemBindData> data;
    data.Attach(new BindData(leaf(path)));
    hr = context->RegisterObjectParam(const_cast<wchar_t *>(STR_FILE_SYS_BIND_DATA), data.Get());
    return FAILED(hr) ? hr
                      : SHCreateItemFromParsingName(path.c_str(), context.Get(), IID_PPV_ARGS(out));
}
HRESULT shellArray(const std::vector<std::wstring> &paths, IShellItemArray **out) {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (paths.empty())
        return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> ids;
    HRESULT hr = S_OK;
    for (auto &path : paths) {
        ComPtr<IShellItem> item;
        hr = shellItem(path, &item);
        if (FAILED(hr))
            break;
        PIDLIST_ABSOLUTE id = nullptr;
        hr = SHGetIDListFromObject(item.Get(), &id);
        if (FAILED(hr))
            break;
        ids.push_back(id);
    }
    if (SUCCEEDED(hr))
        hr = SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE *)ids.data(),
                                               out);
    for (auto id : ids)
        CoTaskMemFree(id);
    return hr;
}
HRESULT validate(const Request &request, Result &result, std::wstring &message) {
    if (result.paths.empty()) {
        message = L"Choose a file.";
        return S_FALSE;
    }
    if (result.paths.size() > 1 &&
        (!(request.options & FOS_ALLOWMULTISELECT) || request.mode != Mode::Open)) {
        message = L"Choose one file.";
        return S_FALSE;
    }
    for (auto &path : result.paths) {
        try {
            path = full(path, result.folder);
        } catch (...) {
            message = L"The path is invalid.";
            return S_FALSE;
        }
        auto name = leaf(path);
        if (request.mode != Mode::Folder &&
            (name.empty() || name.find_first_of(L"<>:\"/\\|?*") != name.npos ||
             std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }) ||
             name.back() == L'.' || name.back() == L' ')) {
            message = L"Enter a valid filename.";
            return S_FALSE;
        }
        auto device = name.substr(0, name.find(L'.'));
        std::transform(device.begin(), device.end(), device.begin(),
                       [](wchar_t c) { return (wchar_t)towupper(c); });
        if (request.mode != Mode::Folder &&
            (device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL" ||
             (device.size() == 4 &&
              (device.substr(0, 3) == L"COM" || device.substr(0, 3) == L"LPT") &&
              device[3] >= L'1' && device[3] <= L'9'))) {
            message = L"This filename is reserved by Windows.";
            return S_FALSE;
        }
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (request.mode == Mode::Folder) {
            if (!directory(path)) {
                message = L"Choose an existing folder.";
                return S_FALSE;
            }
        } else if (attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            message = L"Choose a file.";
            return S_FALSE;
        }
        if ((request.options & FOS_FILEMUSTEXIST) && attributes == INVALID_FILE_ATTRIBUTES) {
            message = L"The selected file no longer exists.";
            return S_FALSE;
        }
        if ((request.options & FOS_PATHMUSTEXIST) && request.mode != Mode::Folder &&
            !directory(parent(path))) {
            message = L"The destination folder does not exist.";
            return S_FALSE;
        }
        if ((request.options & FOS_NOREADONLYRETURN) && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_READONLY)) {
            message = L"The file is read-only.";
            return S_FALSE;
        }
        if (!(request.options & FOS_NODEREFERENCELINKS) && PathMatchSpecW(path.c_str(), L"*.lnk")) {
            ComPtr<IShellLinkW> link;
            HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&link));
            ComPtr<IPersistFile> file;
            if (SUCCEEDED(hr))
                hr = link.As(&file);
            if (SUCCEEDED(hr))
                hr = file->Load(path.c_str(), STGM_READ);
            if (SUCCEEDED(hr))
                hr = link->Resolve(nullptr, SLR_NO_UI | SLR_NOUPDATE);
            wchar_t target[32768]{};
            if (SUCCEEDED(hr))
                hr = link->GetPath(target, 32768, nullptr, SLGP_RAWPATH);
            if (SUCCEEDED(hr) && target[0])
                path = target;
        }
    }
    return S_OK;
}

HRESULT controlPicker(Shared *shared, HWND window, Request &request, DWORD changes) {
    if (!shared || !window)
        return E_UNEXPECTED;
    try {
        auto data = encode(request);
        memcpy(shared->control, data.data(), data.size());
        shared->controlBytes = (DWORD)data.size();
        MemoryBarrier();
        DWORD_PTR answer = 0;
        if (!SendMessageTimeoutW(window, ControlMessage, changes, 0, SMTO_ABORTIFHUNG, 5000,
                                 &answer) ||
            !answer)
            return E_FAIL;
        MemoryBarrier();
        request = decodeRequest(shared->control, shared->controlBytes);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}
HRESULT runPicker(const std::wstring &executable, Request request, HWND owner,
                  const Callback &callback, Result &result, HANDLE *cancellation,
                  HWND *dialogWindow, Shared **shared, bool manageOwner, HANDLE externalCancellation) {
    if (cancellation)
        *cancellation = nullptr;
    if (dialogWindow)
        *dialogWindow = nullptr;
    if (shared)
        *shared = nullptr;
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        return E_NOTIMPL;
    Handles h;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    h.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                   sizeof(Shared), nullptr);
    h.ready = CreateEventW(&security, TRUE, FALSE, nullptr);
    h.event = CreateEventW(&security, FALSE, FALSE, nullptr);
    h.decision = CreateEventW(&security, FALSE, FALSE, nullptr);
    h.cancel = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!h.mapping || !h.ready || !h.event || !h.decision || !h.cancel)
        return HRESULT_FROM_WIN32(GetLastError());
    h.shared = (Shared *)MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!h.shared)
        return HRESULT_FROM_WIN32(GetLastError());
    memset(h.shared, 0, sizeof(Shared));
    h.shared->magic = Magic;
    h.shared->version = Version;
    h.shared->ownerPid = GetCurrentProcessId();
    h.shared->ownerWindow = (UINT64)owner;
    try {
        auto data = encode(request);
        memcpy(h.shared->request, data.data(), data.size());
        h.shared->requestBytes = (DWORD)data.size();
    } catch (...) {
        return E_INVALIDARG;
    }
    HANDLE inherited[] = {h.mapping, h.ready, h.event, h.decision, h.cancel};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, size);
    if (!attrs)
        return E_OUTOFMEMORY;
    bool initialized = InitializeProcThreadAttributeList(attrs, 1, 0, &size) != FALSE;
    if (!initialized ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr)) {
        auto error = GetLastError();
        if (initialized)
            DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
        return HRESULT_FROM_WIN32(error);
    }
    wchar_t suffix[256];
    swprintf_s(suffix, L" --fp-picker-v1 %llx %llx %llx %llx %llx", (UINT64)h.mapping,
               (UINT64)h.ready, (UINT64)h.event, (UINT64)h.decision, (UINT64)h.cancel);
    std::wstring command = quote(executable) + suffix;
    STARTUPINFOEXW info{};
    info.StartupInfo.cb = sizeof(info);
    info.lpAttributeList = attrs;
    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
                                  parent(executable).c_str(), &info.StartupInfo, &process);
    DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    HeapFree(GetProcessHeap(), 0, attrs);
    if (!created)
        return HRESULT_FROM_WIN32(error);
    AllowSetForegroundWindow(process.dwProcessId);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    bool ownerWasEnabled = manageOwner && owner && IsWindow(owner) && IsWindowEnabled(owner);
    bool shown = false;
    HRESULT hr = E_FAIL;
    ULONGLONG deadline = GetTickCount64() + 15000;
    bool quit = false;
    int quitCode = 0;
    if (cancellation)
        *cancellation = h.cancel;
    if (shared)
        *shared = h.shared;
    for (;;) {
        // A WinRT async operation owns this event for the entire call. Unlike
        // publishing the private cancel handle, this has no close/cancel race.
        if (externalCancellation && WaitForSingleObject(externalCancellation, 0) == WAIT_OBJECT_0) {
            SetEvent(h.cancel);
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            break;
        }
        HANDLE waits[] = {h.event, h.cancel, process.hProcess, h.ready};
        DWORD wait = MsgWaitForMultipleObjectsEx(shown ? 3 : 4, waits, 100, QS_ALLINPUT,
                                                 MWMO_INPUTAVAILABLE);
        if (!shown && (WaitForSingleObject(h.ready, 0) == WAIT_OBJECT_0)) {
            if (h.shared->helperPid != process.dwProcessId) {
                hr = E_ACCESSDENIED;
                break;
            }
            shown = true;
            if (dialogWindow)
                *dialogWindow = (HWND)h.shared->helperWindow;
            if (ownerWasEnabled)
                EnableWindow(owner, FALSE);
        }
        if (wait == WAIT_OBJECT_0) {
            try {
                MemoryBarrier();
                auto current = decodeResult(h.shared->event, h.shared->eventBytes);
                auto kind = (Event)h.shared->kind;
                std::wstring message;
                hr = callback(kind, current, message);
                Writer reply;
                reply.string(message);
                memcpy(h.shared->replyText, reply.data.data(), reply.data.size());
                h.shared->replyBytes = (DWORD)reply.data.size();
                h.shared->reply = hr;
                MemoryBarrier();
                SetEvent(h.decision);
                if (kind == Event::Accept && hr == S_OK) {
                    result = std::move(current);
                    break;
                }
                if (FAILED(hr) && hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                    break;
            } catch (...) {
                hr = E_FAIL;
                break;
            }
        } else if (wait == WAIT_OBJECT_0 + 1) {
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            break;
        } else if (wait == WAIT_OBJECT_0 + 2) {
            hr = shown ? E_FAIL : E_NOTIMPL;
            break;
        } else if (wait == (shown ? WAIT_OBJECT_0 + 3 : WAIT_OBJECT_0 + 4)) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) {
                    quit = true;
                    quitCode = (int)m.wParam;
                    SetEvent(h.cancel);
                    break;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (quit) {
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                break;
            }
        } else if (wait == WAIT_FAILED) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (!shown && GetTickCount64() > deadline) {
            hr = E_NOTIMPL;
            break;
        }
        if ((request.testCancel || !request.testPaths.empty()) &&
            GetTickCount64() > deadline + 15000) {
            hr = E_FAIL;
            break;
        }
        if (owner && !IsWindow(owner)) {
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            break;
        }
    }
    if (cancellation)
        *cancellation = nullptr;
    if (dialogWindow)
        *dialogWindow = nullptr;
    if (shared)
        *shared = nullptr;
    SetEvent(h.cancel);
    if (ownerWasEnabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    // A private helper is owned by this request. Reap it before closing its IPC handles.
    if (WaitForSingleObject(process.hProcess, 2000) == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, ERROR_CANCELLED);
    CloseHandle(process.hProcess);
    if (quit)
        PostQuitMessage(quitCode);
    return hr;
}
} // namespace fpp
