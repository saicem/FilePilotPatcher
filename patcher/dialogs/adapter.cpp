#include "client.h"
#include "modern_dialog.h"
#include "agent_shared.h"
#include "child_delivery.h"
#include <atomic>
#include "vendor/minhook/include/MinHook.h"
#include <cderr.h>
#include <commdlg.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace fpp;
namespace {
std::wstring pickerExecutable;
std::mutex installMutex;
bool installed = false;
std::atomic<bool> agentManaged{false};
std::mutex agentAttachMutex;
HANDLE agentMapping = nullptr;
fpp::agent::Policy *agentPolicy = nullptr;
std::wstring agentImage;
HANDLE diagnosticsMapping = nullptr;
fpp::agent::DialogDiagnostics *diagnostics = nullptr;
SRWLOCK diagnosticsLock = SRWLOCK_INIT;
void markRoute(fpp::agent::DialogRoute route, DWORD detail = 0) {
    if (!agentManaged.load(std::memory_order_acquire) || !diagnostics) return;
    AcquireSRWLockExclusive(&diagnosticsLock);
    InterlockedIncrement(&diagnostics->sequence);
    diagnostics->route = route; diagnostics->detail = detail;
    MemoryBarrier(); InterlockedIncrement(&diagnostics->sequence);
    ReleaseSRWLockExclusive(&diagnosticsLock);
}
bool replacementsEnabled() {
    if (fpp::agent::comModeInstalled()) return false;
    if (!agentManaged.load(std::memory_order_acquire)) return true;
    if (!fpp::agent::policyAlive(agentPolicy) || !agentPolicy->enabled) return false;
    try {
        for (unsigned attempt = 0; attempt != 3; ++attempt) {
            LONG before = agentPolicy->generation;
            if (before & 1) continue;
            MemoryBarrier();
            std::wstring patterns(agentPolicy->exclusions,
                wcsnlen_s(agentPolicy->exclusions, fpp::agent::ExclusionCapacity));
            MemoryBarrier();
            if (before == agentPolicy->generation) return !fpp::agent::excluded(agentImage, patterns);
        }
    } catch (...) {}
    return false;
}
bool childDeliveryEnabled() {
    return agentManaged.load(std::memory_order_acquire) && replacementsEnabled();
}
thread_local unsigned bypass = 0;
thread_local DWORD legacyError = 0;
thread_local bool legacyOwnError = false;
thread_local std::optional<Request> testRequest;
struct Guard {
    Guard() {
        ++bypass;
    }
    ~Guard() {
        --bypass;
    }
};
decltype(&CoCreateInstance) originalCreate;
decltype(&CoGetClassObject) originalFactory;
decltype(&CoCreateInstanceEx) originalCreateEx;
decltype(&CoCreateInstance) originalCombaseCreate;
decltype(&CoGetClassObject) originalCombaseFactory;
decltype(&CoCreateInstanceEx) originalCombaseCreateEx;
decltype(&GetOpenFileNameW) originalOpenW;
decltype(&GetSaveFileNameW) originalSaveW;
decltype(&GetOpenFileNameA) originalOpenA;
decltype(&GetSaveFileNameA) originalSaveA;
decltype(&CommDlgExtendedError) originalError;
using fpp::modern::knownInterface;
fpp::modern::Runtime modernRuntime() {
    fpp::modern::Runtime config;
    config.pickerExecutable = pickerExecutable;
    config.enabled = replacementsEnabled;
    config.markRoute = markRoute;
    config.enterShow = [] { ++bypass; };
    config.leaveShow = [] { --bypass; };
    config.prepareRequest = [](Request &request) {
        if (testRequest) {
            request.testPaths = std::move(testRequest->testPaths);
            request.testCancel = testRequest->testCancel;
            testRequest.reset();
        }
    };
    return config;
}
HRESULT wrap(IUnknown *object, bool save, REFIID id, void **out) {
    return fpp::modern::wrap(object, save, id, out, modernRuntime());
}
HRESULT createCore(decltype(&CoCreateInstance) original, REFCLSID cls, LPUNKNOWN outer,
                   DWORD context, REFIID id, void **out) {
    if (bypass || (cls != CLSID_FileOpenDialog && cls != CLSID_FileSaveDialog))
        return original(cls, outer, context, id, out);
    if (outer || !knownInterface(id, cls == CLSID_FileSaveDialog)) {
        markRoute(outer ? fpp::agent::DialogRoute::NativeActivation :
                          fpp::agent::DialogRoute::NativeInterface, outer ? 1 : id.Data1);
        return original(cls, outer, context, id, out);
    }
    if (!replacementsEnabled()) {
        markRoute(fpp::agent::DialogRoute::NativeDisabled);
        return original(cls, outer, context, id, out);
    }
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Guard guard;
    ComPtr<IUnknown> object;
    auto hr = original(cls, nullptr, context, IID_PPV_ARGS(&object));
    if (SUCCEEDED(hr)) hr = wrap(object.Get(), cls == CLSID_FileSaveDialog, id, out);
    markRoute(FAILED(hr) ? fpp::agent::DialogRoute::Failed : fpp::agent::DialogRoute::Created,
              FAILED(hr) ? (DWORD)hr : 0);
    return hr;
}
HRESULT WINAPI create(REFCLSID cls, LPUNKNOWN outer, DWORD context, REFIID id, void **out) {
    return createCore(originalCreate, cls, outer, context, id, out);
}
HRESULT WINAPI createCombase(REFCLSID cls, LPUNKNOWN outer, DWORD context, REFIID id, void **out) {
    return createCore(originalCombaseCreate, cls, outer, context, id, out);
}
HRESULT createExCore(decltype(&CoCreateInstanceEx) original, REFCLSID cls, IUnknown *outer,
                     DWORD context, COSERVERINFO *server, DWORD count, MULTI_QI *queries) {
    if (bypass || !queries || !count ||
        (cls != CLSID_FileOpenDialog && cls != CLSID_FileSaveDialog))
        return original(cls, outer, context, server, count, queries);
    if (outer || server) {
        markRoute(fpp::agent::DialogRoute::NativeActivation, outer ? 1 : 2);
        return original(cls, outer, context, server, count, queries);
    }
    if (!replacementsEnabled()) {
        markRoute(fpp::agent::DialogRoute::NativeDisabled);
        return original(cls, outer, context, server, count, queries);
    }
    bool save = cls == CLSID_FileSaveDialog;
    for (DWORD i = 0; i < count; ++i)
        if (!queries[i].pIID || !knownInterface(*queries[i].pIID, save)) {
            markRoute(fpp::agent::DialogRoute::NativeInterface, queries[i].pIID ? queries[i].pIID->Data1 : 0);
            return original(cls, outer, context, server, count, queries);
        }
    for (DWORD i = 0; i < count; ++i) { queries[i].pItf = nullptr; queries[i].hr = E_NOINTERFACE; }
    Guard guard;
    MULTI_QI base{&IID_IUnknown, nullptr, E_NOINTERFACE};
    auto hr = original(cls, nullptr, context, nullptr, 1, &base);
    ComPtr<IUnknown> object; object.Attach(base.pItf);
    if (FAILED(hr) || FAILED(base.hr)) {
        hr = FAILED(hr) ? hr : base.hr;
        for (DWORD i = 0; i < count; ++i) queries[i].hr = hr;
        markRoute(fpp::agent::DialogRoute::Failed, (DWORD)hr);
        return hr;
    }
    ComPtr<IUnknown> proxy;
    hr = wrap(object.Get(), save, IID_PPV_ARGS(&proxy));
    if (FAILED(hr)) return hr;
    DWORD succeeded = 0;
    for (DWORD i = 0; i < count; ++i) {
        queries[i].hr = proxy->QueryInterface(*queries[i].pIID, (void **)&queries[i].pItf);
        if (SUCCEEDED(queries[i].hr)) ++succeeded;
    }
    proxy.Reset();
    hr = succeeded == count ? S_OK : succeeded ? CO_S_NOTALLINTERFACES : E_NOINTERFACE;
    markRoute(FAILED(hr) ? fpp::agent::DialogRoute::Failed : fpp::agent::DialogRoute::Created,
              FAILED(hr) ? (DWORD)hr : 0);
    return hr;
}
HRESULT WINAPI createEx(REFCLSID cls, IUnknown *outer, DWORD context, COSERVERINFO *server,
                       DWORD count, MULTI_QI *queries) {
    return createExCore(originalCreateEx, cls, outer, context, server, count, queries);
}
HRESULT WINAPI createCombaseEx(REFCLSID cls, IUnknown *outer, DWORD context, COSERVERINFO *server,
                              DWORD count, MULTI_QI *queries) {
    return createExCore(originalCombaseCreateEx, cls, outer, context, server, count, queries);
}
class Factory final : public IClassFactory {
    LONG refs = 1;
    ComPtr<IClassFactory> native;
    bool save;

  public:
    Factory(IClassFactory *p, bool s) : native(p), save(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IClassFactory)
            return E_NOINTERFACE;
        *out = (IClassFactory *)this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = InterlockedDecrement(&refs);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID id, void **out) override {
        if (bypass)
            return native->CreateInstance(outer, id, out);
        if (outer || !knownInterface(id, save)) {
            markRoute(outer ? fpp::agent::DialogRoute::NativeActivation :
                              fpp::agent::DialogRoute::NativeInterface, outer ? 1 : id.Data1);
            return native->CreateInstance(outer, id, out);
        }
        if (!replacementsEnabled()) {
            markRoute(fpp::agent::DialogRoute::NativeDisabled);
            return native->CreateInstance(outer, id, out);
        }
        Guard guard;
        ComPtr<IUnknown> object;
        auto hr = native->CreateInstance(nullptr, IID_PPV_ARGS(&object));
        if (SUCCEEDED(hr)) hr = wrap(object.Get(), save, id, out);
        markRoute(FAILED(hr) ? fpp::agent::DialogRoute::Failed : fpp::agent::DialogRoute::Created,
                  FAILED(hr) ? (DWORD)hr : 0);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        return native->LockServer(lock);
    }
};
HRESULT factoryCore(decltype(&CoGetClassObject) original, REFCLSID cls, DWORD context,
                    LPVOID server, REFIID id, void **out) {
    if (bypass || (cls != CLSID_FileOpenDialog && cls != CLSID_FileSaveDialog))
        return original(cls, context, server, id, out);
    if (id != IID_IUnknown && id != IID_IClassFactory) {
        markRoute(fpp::agent::DialogRoute::NativeInterface, id.Data1);
        return original(cls, context, server, id, out);
    }
    if (!replacementsEnabled()) {
        markRoute(fpp::agent::DialogRoute::NativeDisabled);
        return original(cls, context, server, id, out);
    }
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Guard guard;
    ComPtr<IClassFactory> object;
    auto hr = original(cls, context, server, IID_PPV_ARGS(&object));
    if (FAILED(hr)) {
        markRoute(fpp::agent::DialogRoute::Failed, (DWORD)hr);
        return hr;
    }
    auto proxy = new Factory(object.Get(), cls == CLSID_FileSaveDialog);
    hr = proxy->QueryInterface(id, out);
    proxy->Release();
    markRoute(FAILED(hr) ? fpp::agent::DialogRoute::Failed : fpp::agent::DialogRoute::FactoryCreated,
              FAILED(hr) ? (DWORD)hr : 0);
    return hr;
}
HRESULT WINAPI factory(REFCLSID cls, DWORD context, LPVOID server, REFIID id, void **out) {
    return factoryCore(originalFactory, cls, context, server, id, out);
}
HRESULT WINAPI factoryCombase(REFCLSID cls, DWORD context, LPVOID server, REFIID id, void **out) {
    return factoryCore(originalCombaseFactory, cls, context, server, id, out);
}

std::wstring wide(const char *text) {
    if (!text)
        return {};
    auto n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring s(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, s.data(), n);
    if (n)
        s.resize(n - 1);
    return s;
}
std::string ansi(const std::wstring &value) {
    BOOL lossy = FALSE;
    auto codepage = GetACP();
    auto flags = codepage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    auto lossFlag = codepage == CP_UTF8 ? nullptr : &lossy;
    auto n = WideCharToMultiByte(codepage, flags, value.c_str(), (int)value.size(), nullptr, 0,
                                 nullptr, lossFlag);
    if (!n && !value.empty())
        throw std::runtime_error("path cannot be encoded");
    std::string out(n, '\0');
    WideCharToMultiByte(codepage, flags, value.c_str(), (int)value.size(), out.data(), n, nullptr,
                        lossFlag);
    if (lossy)
        throw std::runtime_error("path cannot be encoded");
    return out;
}
template <class T> std::wstring string(const T *value) {
    if constexpr (sizeof(T) == 2)
        return value ? std::wstring((const wchar_t *)value) : L"";
    else
        return wide((const char *)value);
}
template <class T> std::basic_string<T> output(const std::wstring &value) {
    if constexpr (sizeof(T) == 2)
        return {(const T *)value.data(), value.size()};
    else
        return ansi(value);
}
template <class OFN> BOOL legacy(OFN *ofn, bool save) {
    using T = std::remove_pointer_t<decltype(ofn->lpstrFile)>;
    auto native = [&](fpp::agent::DialogRoute reason = fpp::agent::DialogRoute::NativeLegacy, DWORD detail = 0) {
        if (!bypass || reason == fpp::agent::DialogRoute::NativeHelper)
            markRoute(replacementsEnabled() ? reason : fpp::agent::DialogRoute::NativeDisabled, detail);
        legacyOwnError = false;
        Guard guard;
        if constexpr (sizeof(T) == 2)
            return save ? originalSaveW((LPOPENFILENAMEW)ofn) : originalOpenW((LPOPENFILENAMEW)ofn);
        else
            return save ? originalSaveA((LPOPENFILENAMEA)ofn) : originalOpenA((LPOPENFILENAMEA)ofn);
    };
    if (bypass || !replacementsEnabled() || !ofn || ofn->lStructSize != sizeof(OFN) || !ofn->lpstrFile || !ofn->nMaxFile ||
        (ofn->Flags &
         (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE | OFN_NOVALIDATE |
          OFN_SHAREAWARE | OFN_ENABLEINCLUDENOTIFY | OFN_SHOWHELP | OFN_FORCESHOWHIDDEN)) ||
        ((ofn->Flags & OFN_ALLOWMULTISELECT) && !(ofn->Flags & OFN_EXPLORER)) ||
        ofn->lpstrCustomFilter)
        return native();
    Guard guard;
    legacyOwnError = true;
    legacyError = 0;
    Request q;
    q.mode = save ? Mode::Save : Mode::Open;
    q.options = 0;
    q.showReadOnly = !save && !(ofn->Flags & OFN_HIDEREADONLY);
    q.readOnly = (ofn->Flags & OFN_READONLY) != 0;
    for (auto pair : {std::pair<DWORD, DWORD>{OFN_OVERWRITEPROMPT, FOS_OVERWRITEPROMPT},
                      {OFN_NOCHANGEDIR, FOS_NOCHANGEDIR},
                      {OFN_ALLOWMULTISELECT, FOS_ALLOWMULTISELECT},
                      {OFN_PATHMUSTEXIST, FOS_PATHMUSTEXIST},
                      {OFN_FILEMUSTEXIST, FOS_FILEMUSTEXIST},
                      {OFN_CREATEPROMPT, FOS_CREATEPROMPT},
                      {OFN_NOREADONLYRETURN, FOS_NOREADONLYRETURN},
                      {OFN_NOTESTFILECREATE, FOS_NOTESTFILECREATE},
                      {OFN_NODEREFERENCELINKS, FOS_NODEREFERENCELINKS},
                      {OFN_DONTADDTORECENT, FOS_DONTADDTORECENT},
                      {OFN_FORCESHOWHIDDEN, FOS_FORCESHOWHIDDEN}})
        if (ofn->Flags & pair.first)
            q.options |= pair.second;
    try {
        q.title = string(ofn->lpstrTitle);
        q.filename = string(ofn->lpstrFile);
        q.folder = string(ofn->lpstrInitialDir);
        q.extension = string(ofn->lpstrDefExt);
        q.index = ofn->nFilterIndex ? ofn->nFilterIndex : 1;
        if (!parent(q.filename).empty()) {
            q.folder = parent(q.filename);
            q.filename = leaf(q.filename);
        }
        if (q.folder.empty()) {
            wchar_t cwd[32768];
            GetCurrentDirectoryW(32768, cwd);
            q.folder = cwd;
        }
        if (ofn->lpstrFilter) {
            auto p = ofn->lpstrFilter;
            while (*p && q.filters.size() < 128) {
                auto label = string(p);
                p += std::char_traits<T>::length(p) + 1;
                auto pattern = string(p);
                p += std::char_traits<T>::length(p) + 1;
                q.filters.push_back({label, pattern});
            }
        }
        if (testRequest) {
            q.testPaths = std::move(testRequest->testPaths);
            q.testCancel = testRequest->testCancel;
            testRequest.reset();
        }
        Result result;
        markRoute(fpp::agent::DialogRoute::FilePilot);
        auto hr = runPicker(
            pickerExecutable, q, ofn->hwndOwner,
            [&](Event e, Result &r, std::wstring &message) {
                if (e != Event::Accept)
                    return S_OK;
                auto valid = validate(q, r, message);
                if (valid != S_OK)
                    return valid;
                if (save && (q.options & FOS_OVERWRITEPROMPT) &&
                    GetFileAttributesW(r.paths.front().c_str()) != INVALID_FILE_ATTRIBUTES &&
                    MessageBoxW(ofn->hwndOwner,
                                (L"Replace the existing file?\n\n" + r.paths.front()).c_str(),
                                L"FilePilot — Confirm Save",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                    return S_FALSE;
                return S_OK;
            },
            result);
        if (hr == E_NOTIMPL && !q.testCancel && q.testPaths.empty()) {
            return native(fpp::agent::DialogRoute::NativeHelper, (DWORD)hr);
        }
        if (FAILED(hr)) {
            if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) markRoute(fpp::agent::DialogRoute::Failed, (DWORD)hr);
            if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                legacyError = CDERR_DIALOGFAILURE;
            return FALSE;
        }
        std::basic_string<T> buffer;
        if (result.paths.size() == 1)
            buffer = output<T>(result.paths.front());
        else {
            auto dir = parent(result.paths.front());
            buffer = output<T>(dir);
            buffer.push_back(0);
            for (auto &p : result.paths) {
                if (_wcsicmp(parent(p).c_str(), dir.c_str())) {
                    legacyError = CDERR_DIALOGFAILURE;
                    return FALSE;
                }
                buffer += output<T>(leaf(p));
                buffer.push_back(0);
            }
        }
        buffer.push_back(0);
        if (buffer.size() > ofn->nMaxFile) {
            legacyError = FNERR_BUFFERTOOSMALL;
            WORD needed = (WORD)std::min<size_t>(buffer.size(), 65535);
            if (ofn->nMaxFile * sizeof(T) >= 2)
                memcpy(ofn->lpstrFile, &needed, 2);
            return FALSE;
        }
        memcpy(ofn->lpstrFile, buffer.data(), buffer.size() * sizeof(T));
        ofn->nFilterIndex = result.index;
        if (result.readOnly)
            ofn->Flags |= OFN_READONLY;
        else
            ofn->Flags &= ~OFN_READONLY;
        auto selectedExtension = leaf(result.paths.front());
        auto extensionDot = selectedExtension.find_last_of(L'.');
        selectedExtension = extensionDot == selectedExtension.npos
                                ? L""
                                : selectedExtension.substr(extensionDot + 1);
        ofn->Flags &= ~OFN_EXTENSIONDIFFERENT;
        if (!q.extension.empty() && _wcsicmp(selectedExtension.c_str(), q.extension.c_str()))
            ofn->Flags |= OFN_EXTENSIONDIFFERENT;
        auto first = output<T>(result.paths.front());
        auto slash = first.find_last_of(output<T>(L"\\/"));
        ofn->nFileOffset =
            (WORD)(result.paths.size() > 1 ? output<T>(parent(result.paths.front())).size() + 1
                   : slash == first.npos   ? 0
                                           : slash + 1);
        auto dot = first.find_last_of((T)'.');
        ofn->nFileExtension =
            (WORD)(result.paths.size() > 1 || dot == first.npos || dot < ofn->nFileOffset
                       ? 0
                       : dot + 1);
        if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) {
            auto title = output<T>(leaf(result.paths.front()));
            auto n = std::min<size_t>(title.size(), ofn->nMaxFileTitle - 1);
            memcpy(ofn->lpstrFileTitle, title.data(), n * sizeof(T));
            ofn->lpstrFileTitle[n] = 0;
        }
        if (!(q.options & FOS_DONTADDTORECENT))
            for (auto &p : result.paths)
                SHAddToRecentDocs(SHARD_PATHW, p.c_str());
        return TRUE;
    } catch (...) {
        legacyError = CDERR_DIALOGFAILURE;
        return FALSE;
    }
}
BOOL WINAPI openW(LPOPENFILENAMEW p) {
    return legacy(p, false);
}
BOOL WINAPI saveW(LPOPENFILENAMEW p) {
    return legacy(p, true);
}
BOOL WINAPI openA(LPOPENFILENAMEA p) {
    return legacy(p, false);
}
BOOL WINAPI saveA(LPOPENFILENAMEA p) {
    return legacy(p, true);
}
DWORD WINAPI extendedError() {
    return legacyOwnError ? legacyError : originalError();
}
bool hook(HMODULE module, const char *name, void *replacement, void **original) {
    auto address = GetProcAddress(module, name);
    return address && MH_CreateHook((void *)address, replacement, original) == MH_OK;
}
bool hookCombase(HMODULE ole, HMODULE combase, const char *name, void *replacement, void **original) {
    if (!combase) return false;
    auto address = GetProcAddress(combase, name);
    if (address && address == GetProcAddress(ole, name)) return true;
    return hook(combase, name, replacement, original);
}
} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI FilePilotInstall(void *path) {
    std::lock_guard<std::mutex> lock(installMutex);
    if (installed)
        return ERROR_SUCCESS;
    if (!path)
        return ERROR_INVALID_PARAMETER;
    pickerExecutable = (const wchar_t *)path;
    if (PathIsRelativeW(pickerExecutable.c_str()))
        return ERROR_INVALID_PARAMETER;
    if (GetFileAttributesW(pickerExecutable.c_str()) == INVALID_FILE_ATTRIBUTES)
        return ERROR_FILE_NOT_FOUND;
    auto ole = LoadLibraryW(L"ole32.dll"), commdlg = LoadLibraryW(L"comdlg32.dll");
    auto combase = LoadLibraryW(L"combase.dll");
    if (!ole || !commdlg || !combase) return ERROR_MOD_NOT_FOUND;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return ERROR_INVALID_FUNCTION;
    if (!hook(ole, "CoCreateInstance", (void *)create, (void **)&originalCreate) ||
        !hook(ole, "CoGetClassObject", (void *)factory, (void **)&originalFactory) ||
        !hook(ole, "CoCreateInstanceEx", (void *)createEx, (void **)&originalCreateEx) ||
        !hookCombase(ole, combase, "CoCreateInstance", (void *)createCombase, (void **)&originalCombaseCreate) ||
        !hookCombase(ole, combase, "CoGetClassObject", (void *)factoryCombase, (void **)&originalCombaseFactory) ||
        !hookCombase(ole, combase, "CoCreateInstanceEx", (void *)createCombaseEx, (void **)&originalCombaseCreateEx) ||
        !hook(commdlg, "GetOpenFileNameW", (void *)openW, (void **)&originalOpenW) ||
        !hook(commdlg, "GetSaveFileNameW", (void *)saveW, (void **)&originalSaveW) ||
        !hook(commdlg, "GetOpenFileNameA", (void *)openA, (void **)&originalOpenA) ||
        !hook(commdlg, "GetSaveFileNameA", (void *)saveA, (void **)&originalSaveA) ||
        !hook(commdlg, "CommDlgExtendedError", (void *)extendedError, (void **)&originalError) ||
        !fpp::agent::installChildDeliveryHooks(childDeliveryEnabled)) {
        MH_Uninitialize();
        return ERROR_INVALID_FUNCTION;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        return ERROR_INVALID_FUNCTION;
    }
    installed = true;
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI FilePilotAgentAttach(void *parameter) {
    using namespace fpp::agent;
    auto reply = (AttachReply *)parameter;
    if (!reply || reply->bytes != sizeof(AttachReply) || reply->version != AgentVersion)
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lock(agentAttachMutex);
        if (!agentPolicy) {
            wchar_t image[PathCapacity]; DWORD count = (DWORD)PathCapacity;
            if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, image, &count)) {
                reply->error = GetLastError(); return reply->error;
            }
            agentImage.assign(image, count);
            agentPolicy = openPolicy(agentMapping);
            if (!agentPolicy) { reply->error = ERROR_NOT_READY; return reply->error; }
        }
        if (!policyAlive(agentPolicy)) { reply->error = ERROR_NOT_READY; return reply->error; }
        size_t length = wcsnlen_s(agentPolicy->picker, PathCapacity);
        if (!length || length == PathCapacity) { reply->error = ERROR_BAD_PATHNAME; return reply->error; }
        auto error = FilePilotInstall(agentPolicy->picker);
        reply->error = error; reply->adapterVersion = AgentVersion;
        if (!error) {
            if (!diagnostics && !agentManaged.load(std::memory_order_acquire)) {
                FILETIME creation{}, exit{}, kernel{}, user{};
                if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
                    ULONGLONG stamp = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
                    diagnosticsMapping = createMapping(diagnosticName(GetCurrentProcessId(), stamp), sizeof(DialogDiagnostics));
                    if (diagnosticsMapping) {
                        diagnostics = (DialogDiagnostics *)MapViewOfFile(diagnosticsMapping, FILE_MAP_WRITE, 0, 0, sizeof(DialogDiagnostics));
                        if (diagnostics) {
                            diagnostics->version = AgentVersion; diagnostics->bytes = sizeof(DialogDiagnostics);
                        }
                    }
                }
            }
            agentManaged.store(true, std::memory_order_release);
            reply->hooks = HookCom | HookFactory | HookLegacy | HookComEx | HookChildren;
        }
        return error;
    } catch (...) { reply->error = ERROR_NOT_ENOUGH_MEMORY; return reply->error; }
}
extern "C" __declspec(dllexport) HRESULT WINAPI FilePilotTestNext(const BYTE *packet, DWORD bytes) {
    if (!installed)
        return E_UNEXPECTED;
    try {
        testRequest = decodeRequest(packet, bytes);
        return S_OK;
    } catch (...) {
        return E_INVALIDARG;
    }
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
