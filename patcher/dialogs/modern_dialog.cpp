#include "modern_dialog.h"
#include <memory>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace fpp;
namespace fpp::modern {
std::wstring itemPath(IShellItem *item) {
    PWSTR p = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
        return {};
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}
HRESULT allocate(const std::wstring &s, PWSTR *out) {
    if (!out)
        return E_POINTER;
    *out = (PWSTR)CoTaskMemAlloc((s.size() + 1) * 2);
    if (!*out)
        return E_OUTOFMEMORY;
    memcpy(*out, s.c_str(), (s.size() + 1) * 2);
    return S_OK;
}
constexpr DWORD supportedOptions = FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES | FOS_NOCHANGEDIR |
                                   FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT |
                                   FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_CREATEPROMPT |
                                   FOS_NOREADONLYRETURN | FOS_NOTESTFILECREATE |
                                   FOS_DONTADDTORECENT | FOS_NODEREFERENCELINKS;

class Dialog final : public IFileOpenDialog,
                     public IFileSaveDialog,
                     public IFileDialog2,
                     public IFileDialogCustomize,
                     public IOleWindow {
    Runtime runtime;
    LONG references = 1;
    ComPtr<IFileDialog> native;
    Request q;
    Result state;
    bool save = false, fallback = false, showing = false, accepted = false, candidate = false,
         explicitFolder = false;
    DWORD fallbackDetail = 0;
    bool readOnlyCollection = false;
    std::vector<PROPERTYKEY> collectedKeys;
    void useNative(fpp::agent::NativeConfiguration reason) {
        fallback = true;
        if (!fallbackDetail) fallbackDetail = (DWORD)reason;
    }
    HANDLE cancelHandle = nullptr;
    HWND helper = nullptr;
    Shared *session = nullptr;
    bool closeRequested = false;
    HRESULT closed = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    ComPtr<IShellItem> one, current;
    ComPtr<IShellItemArray> many;
    struct Sink {
        DWORD cookie;
        ComPtr<IFileDialogEvents> events;
    };
    std::vector<Sink> sinks;
    GUID clientGuid{};
    bool hasGuid = false;
    IFileDialog *self() {
        return save ? static_cast<IFileDialog *>(static_cast<IFileSaveDialog *>(this))
                    : static_cast<IFileDialog *>(static_cast<IFileOpenDialog *>(this));
    }
    ComPtr<IFileDialogCustomize> customize() {
        if (showing || accepted)
            return {};
        useNative(fpp::agent::NativeConfiguration::CustomControls);
        ComPtr<IFileDialogCustomize> p;
        native.As(&p);
        return p;
    }
    template <class F> HRESULT saveCall(F f) {
        if (!fallback && (showing || accepted))
            return E_NOTIMPL;
        useNative(fpp::agent::NativeConfiguration::SaveMetadata);
        ComPtr<IFileSaveDialog> p;
        auto hr = native.As(&p);
        return FAILED(hr) ? hr : f(p.Get());
    }
    HRESULT update(DWORD changes) {
        return showing && !fallback ? controlPicker(session, helper, q, changes) : S_OK;
    }
    std::wstring historyFile() {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
            return {};
        auto dir = std::wstring(p) + L"\\Voidstar\\FilePilotPicker";
        CoTaskMemFree(p);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        return dir + L"\\dialog-history.ini";
    }
    std::wstring historyKey() {
        wchar_t exe[32768], guid[40];
        GetModuleFileNameW(nullptr, exe, 32768);
        StringFromGUID2(clientGuid, guid, 40);
        return std::wstring(exe) + L"|" + guid + (save ? L"|Save" : L"|Open");
    }
    HRESULT callbacks(Event kind, Result &result, std::wstring &message) {
        auto copy = sinks; // Callback code may Unadvise or release its own references.
        if (kind == Event::Folder) {
            ComPtr<IShellItem> folder;
            auto hr = shellItem(result.folder, &folder);
            if (FAILED(hr))
                return hr;
            for (auto &s : copy) {
                hr = s.events->OnFolderChanging(self(), folder.Get());
                if (hr != S_OK && hr != E_NOTIMPL) {
                    message = L"The application rejected this folder.";
                    return S_FALSE;
                }
            }
            state = result;
            for (auto &s : copy)
                s.events->OnFolderChange(self());
            return S_OK;
        }
        state = result;
        if (kind == Event::Selection) {
            current.Reset();
            if (!result.paths.empty())
                shellItem(result.paths.front(), &current);
            for (auto &s : copy)
                s.events->OnSelectionChange(self());
            return S_OK;
        }
        if (kind == Event::Type) {
            q.index = result.index;
            for (auto &s : copy)
                s.events->OnTypeChange(self());
            return S_OK;
        }
        if (kind != Event::Accept)
            return S_OK;
        HRESULT hr = validate(q, result, message);
        if (hr != S_OK)
            return hr;
        state = result;
        one.Reset();
        many.Reset();
        if (state.paths.size() == 1) {
            hr = shellItem(state.paths.front(), &one);
            if (FAILED(hr)) {
                message = L"Windows could not represent the selected file.";
                return S_FALSE;
            }
        }
        if (!save) {
            hr = shellArray(state.paths, &many);
            if (FAILED(hr)) {
                message = L"Windows could not represent this selection.";
                return S_FALSE;
            }
        }
        candidate = true;
        if (save && (q.options & FOS_OVERWRITEPROMPT) &&
            GetFileAttributesW(state.paths.front().c_str()) != INVALID_FILE_ATTRIBUTES) {
            FDE_OVERWRITE_RESPONSE response = FDEOR_DEFAULT;
            for (auto &s : copy) {
                FDE_OVERWRITE_RESPONSE value = FDEOR_DEFAULT;
                s.events->OnOverwrite(self(), one.Get(), &value);
                if (value != FDEOR_DEFAULT)
                    response = value;
            }
            bool allow =
                response == FDEOR_ACCEPT ||
                (response == FDEOR_DEFAULT &&
                 MessageBoxW(helper,
                             (L"Replace the existing file?\n\n" + state.paths.front()).c_str(),
                             L"FilePilot — Confirm Save",
                             MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES);
            if (!allow) {
                candidate = false;
                message = L"Choose another filename or confirm replacement.";
                return S_FALSE;
            }
        }
        if (!save && (q.options & FOS_CREATEPROMPT) &&
            GetFileAttributesW(state.paths.front().c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (MessageBoxW(helper,
                            L"This file does not exist. Return this filename to the application?",
                            L"FilePilot — Open", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                candidate = false;
                return S_FALSE;
            }
        }
        for (auto &s : copy) {
            hr = s.events->OnFileOk(self());
            // Event sinks can implement only part of IFileDialogEvents. Chromium's
            // auto-close sink returns E_NOTIMPL here; it is not a selection veto.
            if (hr != S_OK && hr != E_NOTIMPL) {
                candidate = false;
                message = L"The application rejected this file selection.";
                return S_FALSE;
            }
        }
        if (cancelHandle && WaitForSingleObject(cancelHandle, 0) == WAIT_OBJECT_0) {
            candidate = false;
            return closed;
        }
        accepted = true;
        candidate = false;
        q.index = state.index;
        if (!(q.options & FOS_DONTADDTORECENT))
            for (auto &path : state.paths)
                SHAddToRecentDocs(SHARD_PATHW, path.c_str());
        return S_OK;
    }

  public:
    Dialog(IFileDialog *p, bool isSave, Runtime config)
        : runtime(std::move(config)), native(p), save(isSave) {
        q.mode = save ? Mode::Save : Mode::Open;
        native->GetOptions(&q.options);
        if (runtime.retainModule) runtime.retainModule();
    }
    ~Dialog() {
        for (auto &s : sinks)
            native->Unadvise(s.cookie);
        sinks.clear();
        native.Reset();
        if (runtime.releaseModule) runtime.releaseModule();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IModalWindow || id == IID_IFileDialog)
            *out = self();
        else if (id == IID_IFileOpenDialog && !save)
            *out = (IFileOpenDialog *)this;
        else if (id == IID_IFileSaveDialog && save)
            *out = (IFileSaveDialog *)this;
        else if (id == IID_IFileDialog2)
            *out = (IFileDialog2 *)this;
        else if (id == IID_IFileDialogCustomize)
            *out = (IFileDialogCustomize *)this;
        else if (id == IID_IOleWindow)
            *out = (IOleWindow *)this;
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&references);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = InterlockedDecrement(&references);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Show(HWND owner) override {
        if (showing)
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        AddRef(); // A modal callback may release the application's last reference.
        struct ActiveShow {
            Dialog *dialog;
            ~ActiveShow() {
                dialog->showing = false;
                if (dialog->runtime.leaveShow) dialog->runtime.leaveShow();
                dialog->Release();
            }
        } active{this};
        if (runtime.enterShow) runtime.enterShow();
        try {
            showing = true;
            accepted = candidate = closeRequested = false;
            one.Reset();
            many.Reset();
            current.Reset();
            state = {};
            if (!explicitFolder && hasGuid) {
                wchar_t folder[32768];
                auto key = historyKey();
                GetPrivateProfileStringW(L"Folders", key.c_str(), L"", folder, 32768,
                                         historyFile().c_str());
                if (directory(folder))
                    q.folder = folder;
            }
            if (q.folder.empty()) {
                ComPtr<IShellItem> folder;
                native->GetFolder(&folder);
                q.folder = itemPath(folder.Get());
                if (q.folder.empty()) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) {
                        q.folder = p;
                        CoTaskMemFree(p);
                    }
                }
            }
            q.mode = save ? Mode::Save : (q.options & FOS_PICKFOLDERS) ? Mode::Folder : Mode::Open;
            if (runtime.prepareRequest) runtime.prepareRequest(q);
            HRESULT hr;
            bool enabled = !runtime.enabled || runtime.enabled();
            if (fallback || !enabled || (q.options & ~supportedOptions)) {
                markRoute(!enabled ? fpp::agent::DialogRoute::NativeDisabled : fallback ?
                    fpp::agent::DialogRoute::NativeConfiguration : fpp::agent::DialogRoute::NativeOptions,
                    fallback ? fallbackDetail : q.options & ~supportedOptions);
                fallback = true;
                hr = native->Show(owner);
            } else {
                markRoute(fpp::agent::DialogRoute::FilePilot);
                hr = runPicker(
                    runtime.pickerExecutable, q, owner,
                    [&](Event e, Result &r, std::wstring &m) { return callbacks(e, r, m); }, state,
                    &cancelHandle, &helper, &session);
                if (hr == E_NOTIMPL && !q.testCancel && q.testPaths.empty()) {
                    markRoute(fpp::agent::DialogRoute::NativeHelper, (DWORD)hr);
                    fallback = true;
                    hr = native->Show(owner);
                }
            }
            if (closeRequested)
                hr = closed;
            if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                markRoute(fpp::agent::DialogRoute::Failed, (DWORD)hr);
            showing = false;
            if (FAILED(hr) || closeRequested) {
                accepted = candidate = false;
                one.Reset();
                many.Reset();
            }
            q.testPaths.clear();
            q.testCancel = false;
            if (SUCCEEDED(hr) && hasGuid && !fallback) {
                auto key = historyKey();
                WritePrivateProfileStringW(L"Folders", key.c_str(), state.folder.c_str(),
                                           historyFile().c_str());
            }
            return hr;
        } catch (const std::bad_alloc &) {
            accepted = candidate = false;
            one.Reset(); many.Reset();
            return E_OUTOFMEMORY;
        } catch (...) {
            accepted = candidate = false;
            one.Reset(); many.Reset();
            return E_FAIL;
        }
    }
    void markRoute(fpp::agent::DialogRoute route, DWORD detail = 0) {
        if (runtime.markRoute) runtime.markRoute(route, detail);
    }
    HRESULT STDMETHODCALLTYPE SetFileTypes(UINT n, const COMDLG_FILTERSPEC *types) override {
        if (showing && !fallback)
            return E_UNEXPECTED;
        auto hr = native->SetFileTypes(n, types);
        if (SUCCEEDED(hr)) {
            q.filters.clear();
            for (UINT i = 0; i < n; i++)
                q.filters.push_back({types[i].pszName, types[i].pszSpec});
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT n) override {
        auto hr = native->SetFileTypeIndex(n);
        if (SUCCEEDED(hr)) {
            q.index = n;
            hr = update(Control::TypeIndex);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT *out) override {
        if (!out)
            return E_POINTER;
        if (fallback)
            return native->GetFileTypeIndex(out);
        if (showing) {
            auto hr = update(Control::Query);
            if (FAILED(hr))
                return hr;
        }
        *out = accepted ? state.index : q.index;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents *sink, DWORD *cookie) override {
        auto hr = native->Advise(sink, cookie);
        if (SUCCEEDED(hr))
            sinks.push_back({*cookie, ComPtr<IFileDialogEvents>(sink)});
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
        auto hr = native->Unadvise(cookie);
        if (SUCCEEDED(hr))
            sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                                       [&](auto &s) { return s.cookie == cookie; }),
                        sinks.end());
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS flags) override {
        if (showing && !fallback)
            return E_UNEXPECTED;
        auto hr = native->SetOptions(flags);
        if (SUCCEEDED(hr))
            q.options = flags;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS *out) override {
        return native->GetOptions(out);
    }
    HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem *item) override {
        auto hr = native->SetDefaultFolder(item);
        if (SUCCEEDED(hr) && !explicitFolder) {
            q.folder = itemPath(item);
            if (q.folder.empty())
                useNative(fpp::agent::NativeConfiguration::VirtualFolder);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFolder(IShellItem *item) override {
        auto value = itemPath(item);
        if (showing && !fallback && value.empty())
            return E_NOTIMPL;
        auto hr = native->SetFolder(item);
        if (SUCCEEDED(hr)) {
            explicitFolder = true;
            q.folder = value;
            if (q.folder.empty())
                useNative(fpp::agent::NativeConfiguration::VirtualFolder);
            else
                hr = update(Control::Folder);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFolder(IShellItem **out) override {
        if (fallback)
            return native->GetFolder(out);
        auto value = state.folder.empty() ? q.folder : state.folder;
        return value.empty() ? native->GetFolder(out) : shellItem(value, out);
    }
    HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (fallback)
            return native->GetCurrentSelection(out);
        return current ? current.CopyTo(out) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR name) override {
        auto hr = native->SetFileName(name);
        if (SUCCEEDED(hr)) {
            q.filename = name;
            hr = update(Control::Name);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR *out) override {
        if (fallback)
            return native->GetFileName(out);
        if (showing && !candidate) {
            auto hr = update(Control::Query);
            if (FAILED(hr))
                return hr;
        }
        return allocate(candidate || accepted ? leaf(state.paths.front()) : q.filename, out);
    }
    HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR title) override {
        auto hr = native->SetTitle(title);
        if (SUCCEEDED(hr)) {
            q.title = title;
            hr = update(Control::Title);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR label) override {
        auto hr = native->SetOkButtonLabel(label);
        if (SUCCEEDED(hr)) {
            q.okLabel = label;
            hr = update(Control::OkLabel);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR label) override {
        if (showing || accepted)
            return E_NOTIMPL;
        useNative(fpp::agent::NativeConfiguration::FileNameLabel);
        return native->SetFileNameLabel(label);
    }
    HRESULT STDMETHODCALLTYPE GetResult(IShellItem **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (fallback)
            return native->GetResult(out);
        return (accepted || candidate) && one ? one.CopyTo(out) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE AddPlace(IShellItem *item, FDAP location) override {
        if (showing || accepted)
            return E_NOTIMPL;
        useNative(fpp::agent::NativeConfiguration::Places);
        return native->AddPlace(item, location);
    }
    HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR extension) override {
        auto hr = native->SetDefaultExtension(extension);
        if (SUCCEEDED(hr)) {
            q.extension = extension;
            hr = update(Control::Extension);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Close(HRESULT hr) override {
        closed = hr;
        if (cancelHandle) {
            closeRequested = true;
            SetEvent(cancelHandle);
            return S_OK;
        }
        return native->Close(hr);
    }
    HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID guid) override {
        auto hr = native->SetClientGuid(guid);
        if (SUCCEEDED(hr)) {
            clientGuid = guid;
            hasGuid = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ClearClientData() override {
        if (hasGuid) {
            auto key = historyKey();
            WritePrivateProfileStringW(L"Folders", key.c_str(), nullptr, historyFile().c_str());
        }
        return native->ClearClientData();
    }
    HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter *filter) override {
        if (showing || accepted)
            return E_NOTIMPL;
        useNative(fpp::agent::NativeConfiguration::ShellFilter);
        return native->SetFilter(filter);
    }
    HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (fallback) {
            ComPtr<IFileOpenDialog> p;
            auto hr = native.As(&p);
            return FAILED(hr) ? hr : p->GetResults(out);
        }
        return (accepted || candidate) && many ? many.CopyTo(out) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray **out) override {
        if (fallback) {
            ComPtr<IFileOpenDialog> p;
            auto hr = native.As(&p);
            return FAILED(hr) ? hr : p->GetSelectedItems(out);
        }
        return state.paths.empty() ? E_FAIL : shellArray(state.paths, out);
    }
    HRESULT STDMETHODCALLTYPE SetSaveAsItem(IShellItem *item) override {
        return saveCall([&](IFileSaveDialog *p) { return p->SetSaveAsItem(item); });
    }
    HRESULT STDMETHODCALLTYPE SetProperties(IPropertyStore *store) override {
        return saveCall([&](IFileSaveDialog *p) { return p->SetProperties(store); });
    }
    HRESULT STDMETHODCALLTYPE SetCollectedProperties(IPropertyDescriptionList *list,
                                                     BOOL append) override {
        // Paint requests System.Security.EncryptionOwners before every COM Save.
        // Read-only/innate properties do not require a metadata editor. Treating
        // every collection as editable forced native Show and an external flash.
        // Inspect the actual Windows descriptions rather than special-casing an
        // executable or property name. Appended defaults can contain editable
        // properties and therefore still need Windows' original property UI.
        bool passive = !append;
        std::vector<PROPERTYKEY> keys;
        UINT count = 0;
        if (list && (FAILED(list->GetCount(&count)) || count > 128)) passive = false;
        for (UINT i = 0; passive && i < count; ++i) {
            ComPtr<IPropertyDescription> description;
            PROPDESC_TYPE_FLAGS flags = PDTF_DEFAULT;
            PROPERTYKEY key{};
            if (FAILED(list->GetAt(i, IID_PPV_ARGS(&description))) ||
                FAILED(description->GetTypeFlags(static_cast<PROPDESC_TYPE_FLAGS>(
                    PDTF_ISINNATE | PDTF_ISGROUP), &flags))) {
                passive = false;
            } else if (flags & PDTF_ISGROUP) {
                // A group heading has no property value to collect.
            } else if (!(flags & PDTF_ISINNATE) || FAILED(description->GetPropertyKey(&key))) {
                passive = false;
            } else {
                keys.push_back(key);
            }
        }
        if (!passive && !fallback && (showing || accepted)) return E_NOTIMPL;
        ComPtr<IFileSaveDialog> dialog;
        auto hr = native.As(&dialog);
        if (FAILED(hr)) return hr;
        hr = dialog->SetCollectedProperties(list, append);
        if (SUCCEEDED(hr)) {
            if (!passive) useNative(fpp::agent::NativeConfiguration::SaveMetadata);
            readOnlyCollection = passive;
            collectedKeys = std::move(keys);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetProperties(IPropertyStore **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!fallback && readOnlyCollection) {
            if (!(accepted || candidate) || !one) {
                ComPtr<IFileSaveDialog> dialog;
                auto hr = native.As(&dialog);
                return FAILED(hr) ? hr : dialog->GetProperties(out);
            }
            ComPtr<IPropertyStore> values;
            auto hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&values));
            if (FAILED(hr)) return hr;
            ComPtr<IShellItem2> item;
            hr = one.As(&item);
            if (FAILED(hr)) return hr;
            for (auto const& key : collectedKeys) {
                PROPVARIANT value{};
                hr = item->GetProperty(key, &value);
                // A new Save destination has no file-derived values yet. Missing
                // properties stay absent; no metadata or encryption is written.
                if (SUCCEEDED(hr)) hr = values->SetValue(key, value);
                PropVariantClear(&value);
                if (FAILED(hr) && GetFileAttributesW(state.paths.front().c_str()) != INVALID_FILE_ATTRIBUTES)
                    return hr;
            }
            return values.CopyTo(out);
        }
        return saveCall([&](IFileSaveDialog *p) { return p->GetProperties(out); });
    }
    HRESULT STDMETHODCALLTYPE ApplyProperties(IShellItem *item, IPropertyStore *store, HWND owner,
                                              IFileOperationProgressSink *sink) override {
        if (!fallback && readOnlyCollection) {
            ComPtr<IFileSaveDialog> dialog;
            auto hr = native.As(&dialog);
            return FAILED(hr) ? hr : dialog->ApplyProperties(item, store, owner, sink);
        }
        return saveCall(
            [&](IFileSaveDialog *p) { return p->ApplyProperties(item, store, owner, sink); });
    }
    HRESULT STDMETHODCALLTYPE SetCancelButtonLabel(LPCWSTR label) override {
        if (showing || accepted)
            return E_NOTIMPL;
        useNative(fpp::agent::NativeConfiguration::CancelLabel);
        ComPtr<IFileDialog2> p;
        auto hr = native.As(&p);
        return FAILED(hr) ? hr : p->SetCancelButtonLabel(label);
    }
    HRESULT STDMETHODCALLTYPE SetNavigationRoot(IShellItem *item) override {
        if (showing || accepted)
            return E_NOTIMPL;
        useNative(fpp::agent::NativeConfiguration::NavigationRoot);
        ComPtr<IFileDialog2> p;
        auto hr = native.As(&p);
        return FAILED(hr) ? hr : p->SetNavigationRoot(item);
    }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override {
        if (!out)
            return E_POINTER;
        if (helper) {
            *out = helper;
            return S_OK;
        }
        ComPtr<IOleWindow> p;
        auto hr = native.As(&p);
        return FAILED(hr) ? hr : p->GetWindow(out);
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL enter) override {
        ComPtr<IOleWindow> p;
        auto hr = native.As(&p);
        return FAILED(hr) ? hr : p->ContextSensitiveHelp(enter);
    }
#define CUSTOM(method, signature, args)                                                            \
    HRESULT STDMETHODCALLTYPE method signature override {                                          \
        auto p = customize();                                                                      \
        return p ? p->method args : E_NOINTERFACE;                                                 \
    }
    CUSTOM(EnableOpenDropDown, (DWORD id), (id))
    CUSTOM(AddMenu, (DWORD id, LPCWSTR label), (id, label))
    CUSTOM(AddPushButton, (DWORD id, LPCWSTR label), (id, label))
    CUSTOM(AddComboBox, (DWORD id), (id))
    CUSTOM(AddRadioButtonList, (DWORD id), (id))
    CUSTOM(AddCheckButton, (DWORD id, LPCWSTR label, BOOL checked), (id, label, checked))
    CUSTOM(AddEditBox, (DWORD id, LPCWSTR text), (id, text))
    CUSTOM(AddSeparator, (DWORD id), (id))
    CUSTOM(AddText, (DWORD id, LPCWSTR text), (id, text))
    CUSTOM(SetControlLabel, (DWORD id, LPCWSTR label), (id, label))
    CUSTOM(GetControlState, (DWORD id, CDCONTROLSTATEF *controlState), (id, controlState))
    CUSTOM(SetControlState, (DWORD id, CDCONTROLSTATEF controlState), (id, controlState))
    CUSTOM(GetEditBoxText, (DWORD id, LPWSTR *out), (id, out))
    CUSTOM(SetEditBoxText, (DWORD id, LPCWSTR text), (id, text))
    CUSTOM(GetCheckButtonState, (DWORD id, BOOL *out), (id, out))
    CUSTOM(SetCheckButtonState, (DWORD id, BOOL checked), (id, checked))
    CUSTOM(AddControlItem, (DWORD id, DWORD item, LPCWSTR label), (id, item, label))
    CUSTOM(RemoveControlItem, (DWORD id, DWORD item), (id, item))
    CUSTOM(RemoveAllControlItems, (DWORD id), (id))
    CUSTOM(GetControlItemState, (DWORD id, DWORD item, CDCONTROLSTATEF *out), (id, item, out))
    CUSTOM(SetControlItemState, (DWORD id, DWORD item, CDCONTROLSTATEF controlState),
           (id, item, controlState))
    CUSTOM(GetSelectedControlItem, (DWORD id, DWORD *out), (id, out))
    CUSTOM(SetSelectedControlItem, (DWORD id, DWORD item), (id, item))
    CUSTOM(StartVisualGroup, (DWORD id, LPCWSTR label), (id, label))
    CUSTOM(EndVisualGroup, (), ())
    CUSTOM(MakeProminent, (DWORD id), (id))
    CUSTOM(SetControlItemText, (DWORD id, DWORD item, LPCWSTR text), (id, item, text))
#undef CUSTOM
};

bool knownInterface(REFIID id, bool save) {
    return id == IID_IUnknown || id == IID_IModalWindow || id == IID_IFileDialog ||
           id == IID_IFileDialog2 || id == IID_IFileDialogCustomize || id == IID_IOleWindow ||
           (save ? id == IID_IFileSaveDialog : id == IID_IFileOpenDialog);
}
HRESULT wrap(IUnknown *object, bool save, REFIID id, void **out, Runtime runtime) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!object) return E_INVALIDARG;
    try {
        ComPtr<IFileDialog> dialog;
        auto hr = object->QueryInterface(IID_PPV_ARGS(&dialog));
        if (FAILED(hr))
            return hr;
        auto proxy = new Dialog(dialog.Get(), save, std::move(runtime));
        hr = proxy->QueryInterface(id, out);
        proxy->Release();
        return hr;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

} // namespace fpp::modern
