#include "external_dialog.h"
#include "client.h"
#include <UIAutomation.h>
#include <commctrl.h>
#include <dlgs.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <cwctype>
#include <memory>

using Microsoft::WRL::ComPtr;
namespace fpp::external {
namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
ULONGLONG created(HANDLE process) {
    FILETIME start{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &start, &exit, &kernel, &user)) return 0;
    return (ULONGLONG(start.dwHighDateTime) << 32) | start.dwLowDateTime;
}
std::vector<BYTE> tokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!size || !GetTokenInformation(token, kind, bytes.data(), size, &size)) bytes.clear();
    return bytes;
}
DWORD integrity(HANDLE token) {
    auto data = tokenData(token, TokenIntegrityLevel);
    if (data.empty()) return 0;
    auto sid = ((TOKEN_MANDATORY_LABEL *)data.data())->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}
std::wstring className(HWND window) {
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    return name;
}
bool send(HWND window, UINT message, WPARAM w, LPARAM l, DWORD_PTR &answer) {
    return SendMessageTimeoutW(window, message, w, l, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                               500, &answer) != 0;
}
std::wstring text(HWND window) {
    if (!window) return {};
    DWORD_PTR length = 0;
    if (!send(window, WM_GETTEXTLENGTH, 0, 0, length) || length > 32767) return {};
    std::vector<wchar_t> value((size_t)length + 1);
    DWORD_PTR count = 0;
    if (!send(window, WM_GETTEXT, value.size(), (LPARAM)value.data(), count) || count > length) return {};
    return {value.data(), (size_t)count};
}
std::wstring take(BSTR value) {
    std::wstring result = value ? std::wstring(value, SysStringLen(value)) : L"";
    SysFreeString(value);
    return result;
}
std::wstring name(IUIAutomationElement *element) {
    BSTR value = nullptr;
    if (element) element->get_CurrentName(&value);
    return take(value);
}
std::wstring automationId(IUIAutomationElement *element) {
    BSTR value = nullptr;
    if (element && FAILED(element->get_CachedAutomationId(&value))) element->get_CurrentAutomationId(&value);
    return take(value);
}
template<class T> ComPtr<T> pattern(IUIAutomationElement *element, PATTERNID id) {
    ComPtr<T> result;
    if (element) element->GetCurrentPatternAs(id, IID_PPV_ARGS(&result));
    return result;
}
std::wstring value(IUIAutomationElement *element) {
    auto p = pattern<IUIAutomationValuePattern>(element, UIA_ValuePatternId);
    BSTR result = nullptr;
    if (p) p->get_CurrentValue(&result);
    return take(result);
}
std::wstring normalized(std::wstring value) {
    value.erase(std::remove(value.begin(), value.end(), L'&'), value.end());
    auto start = value.find_first_not_of(L" \t\r\n");
    auto end = value.find_last_not_of(L" \t\r\n.\x2026");
    if (start == value.npos || end == value.npos) return {};
    value = value.substr(start, end - start + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return value;
}
bool modeFromCaption(const std::wstring &caption, Mode &mode) {
    auto key = normalized(caption);
    for (auto label : {L"save", L"save as", L"speichern", L"enregistrer", L"guardar", L"salva",
                       L"salvar", L"opslaan", L"zapisz", L"сохранить", L"保存", L"저장"}) {
        if (key == label) { mode = Mode::Save; return true; }
    }
    for (auto label : {L"open", L"öffnen", L"ouvrir", L"abrir", L"apri", L"openen", L"otwórz",
                       L"открыть", L"打开", L"開く", L"열기"}) {
        if (key == label) { mode = Mode::Open; return true; }
    }
    return false;
}
std::wstring filesystemPath(const std::wstring &input) {
    // Address toolbar names often contain a localized prefix. Extract only a
    // rooted filesystem path; shell breadcrumbs and virtual folders aren't paths.
    for (size_t i = 0; i < input.size(); ++i) {
        if ((i + 2 < input.size() && iswalpha(input[i]) && input[i + 1] == L':' &&
             (input[i + 2] == L'\\' || input[i + 2] == L'/')) ||
            (i + 1 < input.size() && input[i] == L'\\' && input[i + 1] == L'\\')) {
            auto result = input.substr(i);
            if (directory(result)) return result;
        }
    }
    return {};
}
std::wstring filterPattern(const std::wstring &label) {
    auto first = label.find_last_of(L'('), last = label.find_last_of(L')');
    if (first == label.npos || last == label.npos || first >= last) return L"*";
    auto result = label.substr(first + 1, last - first - 1);
    result.erase(std::remove_if(result.begin(), result.end(), [](wchar_t c) { return iswspace(c) != 0; }), result.end());
    return result.find(L'*') == result.npos ? L"*" : result;
}
HWND descendant(HWND root, int id, const wchar_t *cls = nullptr) {
    struct Search { int id; const wchar_t *cls; HWND found = nullptr; } search{id, cls};
    EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL {
        auto &s = *(Search *)data;
        if (GetDlgCtrlID(child) == s.id && (!s.cls || className(child) == s.cls)) {
            s.found = child; return FALSE;
        }
        return TRUE;
    }, (LPARAM)&search);
    return search.found;
}
HWND comboEditor(HWND combo) {
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info) && info.hwndItem && className(info.hwndItem) == L"Edit") return info.hwndItem;
    HWND edit = nullptr;
    EnumChildWindows(combo, [](HWND child, LPARAM data) -> BOOL {
        if (className(child) == L"Edit") { *(HWND *)data = child; return FALSE; }
        return TRUE;
    }, (LPARAM)&edit);
    return edit;
}
void diagnostic(JobShared &job, Reason reason, DWORD error = 0) {
    InterlockedExchange(&job.error, (LONG)error);
    InterlockedExchange(&job.reason, (LONG)reason);
}
struct NativeDialog {
    HWND root = nullptr, editWindow = nullptr, typeWindow = nullptr, okWindow = nullptr, cancelWindow = nullptr;
    ComPtr<IUIAutomation> uia;
    ComPtr<IUIAutomationElement> element, edit, type, ok, cancel, readOnly;
    Request request;
    bool nativeTypes = false;
    JobShared *diagnostics = nullptr;
    bool fail(Reason reason, HRESULT error = S_OK) {
        if (diagnostics) diagnostic(*diagnostics, reason, (DWORD)error);
        return false;
    }
    void bindHost(IUIAutomationElement *host, HWND nativeWindow = nullptr) {
        auto id = automationId(host);
        if (id != L"FileNameControlHost" && id != L"FileTypeControlHost") return;
        if (!nativeWindow) {
            UIA_HWND handle = nullptr;
            if (FAILED(host->get_CurrentNativeWindowHandle(&handle))) return;
            nativeWindow = (HWND)handle;
        }
        if (!nativeWindow || !IsChild(root, nativeWindow) || !IsWindowVisible(nativeWindow)) return;
        auto cls = className(nativeWindow);
        if (id == L"FileNameControlHost" && !editWindow && (cls == L"ComboBox" || cls == L"ComboBoxEx32")) {
            auto editor = comboEditor(nativeWindow);
            if (editor && IsChild(root, editor)) {
                editWindow = editor;
                uia->ElementFromHandle(editWindow, &edit);
            }
        } else if (id == L"FileTypeControlHost" && !typeWindow && cls == L"ComboBox" &&
                   (GetWindowLongW(nativeWindow, GWL_STYLE) & 3) == CBS_DROPDOWNLIST) {
            typeWindow = nativeWindow;
            type = host;
        }
    }
    void nativeHosts() {
        std::vector<HWND> children;
        EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL {
            ((std::vector<HWND> *)data)->push_back(child); return TRUE;
        }, (LPARAM)&children);
        for (auto child : children) {
            if (!IsWindowVisible(child)) continue;
            auto cls = className(child);
            if (cls == L"ComboBox" || cls == L"ComboBoxEx32") {
                // Direct host queries work even when the root accessibility
                // snapshot omits these controls or reports them as plain panes.
                ComPtr<IUIAutomationElement> host;
                if (SUCCEEDED(uia->ElementFromHandle(child, &host))) bindHost(host.Get(), child);
            } else if (cls == L"ToolbarWindow32" && request.folder.empty()) {
                request.folder = filesystemPath(text(child));
            }
        }
    }

    bool inspect(HWND hwnd, JobShared &job) {
        diagnostics = &job;
        root = hwnd;
        if (className(root) != L"#32770") return fail(Reason::WindowClass);
        diagnostic(job, Reason::Accessibility);
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            auto hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
            if (FAILED(hr)) return fail(Reason::Accessibility, hr);
        }
        ComPtr<IUIAutomation2> uia2;
        if (SUCCEEDED(uia.As(&uia2))) {
            uia2->put_ConnectionTimeout(1500);
            uia2->put_TransactionTimeout(1500);
        }
        auto hr = uia->ElementFromHandle(root, &element);
        if (FAILED(hr)) return fail(Reason::Accessibility, hr);
        okWindow = descendant(root, IDOK);
        cancelWindow = descendant(root, IDCANCEL);
        editWindow = descendant(root, edt1, L"Edit");
        HWND nameCombo = descendant(root, cmb13);
        if (!editWindow && nameCombo) editWindow = comboEditor(nameCombo);
        if (editWindow) uia->ElementFromHandle(editWindow, &edit);
        if (okWindow) uia->ElementFromHandle(okWindow, &ok);
        if (cancelWindow) uia->ElementFromHandle(cancelWindow, &cancel);
        typeWindow = descendant(root, cmb14, L"ComboBox");
        if (!typeWindow) typeWindow = descendant(root, cmb1, L"ComboBox");
        if (typeWindow) uia->ElementFromHandle(typeWindow, &type);
        diagnostic(job, Reason::Filename);
        nativeHosts();

        diagnostic(job, Reason::AccessibilityTree);
        ComPtr<IUIAutomationCondition> all;
        ComPtr<IUIAutomationCacheRequest> cache;
        ComPtr<IUIAutomationElementArray> elements;
        hr = uia->CreateTrueCondition(&all);
        if (FAILED(hr)) return fail(Reason::AccessibilityTree, hr);
        hr = uia->CreateCacheRequest(&cache);
        if (FAILED(hr)) return fail(Reason::AccessibilityTree, hr);
        cache->AddProperty(UIA_ControlTypePropertyId);
        cache->AddProperty(UIA_AutomationIdPropertyId);
        cache->put_TreeScope(TreeScope_Element);
        hr = element->FindAllBuildCache(TreeScope_Descendants, all.Get(), cache.Get(), &elements);
        if (FAILED(hr)) return fail(Reason::AccessibilityTree, hr);
        int count = 0; elements->get_Length(&count);
        if (count > 4096) return fail(Reason::AccessibilityTree, HRESULT_FROM_WIN32(ERROR_MORE_DATA));
        bool fileView = false;
        bool customOptions = false;
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> item; elements->GetElement(i, &item);
            if (!item) continue;
            CONTROLTYPEID control = 0; item->get_CachedControlType(&control);
            auto id = automationId(item.Get());
            if (id == L"FileNameControlHost" || id == L"FileTypeControlHost") bindHost(item.Get());
            if (control == UIA_CheckBoxControlTypeId || control == UIA_RadioButtonControlTypeId) {
                BOOL offscreen = TRUE;
                item->get_CurrentIsOffscreen(&offscreen);
                if (!offscreen) {
                    if (control == UIA_CheckBoxControlTypeId && id == L"1040") readOnly = item;
                    else customOptions = true;
                }
            }
            if (!ok && id == L"1" && (control == UIA_ButtonControlTypeId || control == UIA_SplitButtonControlTypeId)) ok = item;
            if (!cancel && id == L"2" && control == UIA_ButtonControlTypeId) cancel = item;
            if (!edit && control == UIA_EditControlTypeId && id == L"1152") edit = item;
            if (!edit && control == UIA_ComboBoxControlTypeId && id == L"1148") {
                auto p = pattern<IUIAutomationValuePattern>(item.Get(), UIA_ValuePatternId);
                if (p) edit = item;
                else {
                    ComPtr<IUIAutomationElementArray> children;
                    if (SUCCEEDED(item->FindAll(TreeScope_Descendants, all.Get(), &children))) {
                        int n = 0; children->get_Length(&n);
                        for (int j = 0; j < n && j < 32; ++j) {
                            ComPtr<IUIAutomationElement> child; children->GetElement(j, &child);
                            if (pattern<IUIAutomationValuePattern>(child.Get(), UIA_ValuePatternId)) { edit = child; break; }
                        }
                    }
                }
            }
            if (!type && control == UIA_ComboBoxControlTypeId && (id == L"1149" || id == L"1136")) type = item;
            if ((control == UIA_ListControlTypeId || control == UIA_DataGridControlTypeId) &&
                (id == L"ItemsView" || id == L"1" || id == L"1120" || id == L"listview")) fileView = true;
            if (request.folder.empty() && (control == UIA_ToolBarControlTypeId || control == UIA_ComboBoxControlTypeId)) {
                request.folder = filesystemPath(value(item.Get()));
                if (request.folder.empty()) request.folder = filesystemPath(name(item.Get()));
            }
        }
        // A generic dialog class and an OK button are not enough to claim a window.
        bool shellView = false;
        EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL {
            auto cls = className(child);
            if (cls == L"SHELLDLL_DefView" || cls == L"ShellDll_DefView" || cls == L"DirectUIHWND" ||
                (cls == L"SysListView32" && GetDlgCtrlID(child) == lst1)) *(bool *)data = true;
            return TRUE;
        }, (LPARAM)&shellView);
        if (customOptions) return fail(Reason::CustomOptions);
        if (!fileView && !shellView) return fail(Reason::FileView);
        if (!editWindow && !edit) return fail(Reason::Filename);
        if (!typeWindow && !type) return fail(Reason::FileType);
        if ((!okWindow && !ok) || (!cancelWindow && !cancel)) return fail(Reason::Actions);
        if (readOnly) {
            auto toggle = pattern<IUIAutomationTogglePattern>(readOnly.Get(), UIA_TogglePatternId);
            ToggleState checked = ToggleState_Off;
            if (!toggle) return fail(Reason::ReadOnly, E_NOINTERFACE);
            hr = toggle->get_CurrentToggleState(&checked);
            if (FAILED(hr)) return fail(Reason::ReadOnly, hr);
            request.showReadOnly = true; request.readOnly = checked == ToggleState_On;
        }
        auto caption = okWindow ? text(okWindow) : name(ok.Get());
        if (!modeFromCaption(caption, request.mode)) return fail(Reason::Mode);
        request.okLabel = caption;
        request.title = text(root);
        request.filename = editWindow ? text(editWindow) : value(edit.Get());
        if (!editWindow && !pattern<IUIAutomationValuePattern>(edit.Get(), UIA_ValuePatternId)) return fail(Reason::FilenamePattern, E_NOINTERFACE);
        if ((!okWindow && !pattern<IUIAutomationInvokePattern>(ok.Get(), UIA_InvokePatternId)) ||
            (!cancelWindow && !pattern<IUIAutomationInvokePattern>(cancel.Get(), UIA_InvokePatternId))) return fail(Reason::ActionPattern, E_NOINTERFACE);
        if (request.folder.empty() && PathIsRelativeW(request.filename.c_str()) == FALSE && directory(parent(request.filename)))
            request.folder = parent(request.filename);
        if (request.folder.empty()) {
            PWSTR documents = nullptr;
            hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents);
            if (FAILED(hr)) return fail(Reason::Folder, hr);
            request.folder = documents; CoTaskMemFree(documents);
        }
        if (PathIsRelativeW(request.filename.c_str()) == FALSE) request.filename = leaf(request.filename);
        // Unknown native flags remain the native dialog's responsibility. No
        // overwrite prompt or shortcut resolution is performed in the external layer.
        request.options = FOS_NOCHANGEDIR | FOS_NODEREFERENCELINKS;
        if (request.mode == Mode::Open) request.options |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        diagnostic(job, Reason::Filters);
        if (!readTypes()) return fail(Reason::Filters);
        diagnostic(job, Reason::None);
        return true;
    }
    bool readTypes() {
        DWORD_PTR count = 0, selected = 0;
        if (typeWindow && send(typeWindow, CB_GETCOUNT, 0, 0, count) && count > 0 && count <= 128 &&
            send(typeWindow, CB_GETCURSEL, 0, 0, selected) && selected < count) {
            for (DWORD_PTR i = 0; i < count; ++i) {
                DWORD_PTR length = 0, read = 0;
                if (!send(typeWindow, CB_GETLBTEXTLEN, i, 0, length) || length > 32767) return false;
                std::vector<wchar_t> label((size_t)length + 1);
                if (!send(typeWindow, CB_GETLBTEXT, i, (LPARAM)label.data(), read) || read > length) return false;
                auto display = std::wstring(label.data(), (size_t)read);
                request.filters.push_back({display, filterPattern(display)});
            }
            request.index = (DWORD)selected + 1;
            nativeTypes = true;
            return true;
        }
        auto selection = pattern<IUIAutomationSelectionPattern>(type.Get(), UIA_SelectionPatternId);
        ComPtr<IUIAutomationElementArray> selectedItems;
        std::wstring display = value(type.Get());
        if (selection && SUCCEEDED(selection->GetCurrentSelection(&selectedItems))) {
            int n = 0; selectedItems->get_Length(&n);
            if (n == 1) {
                ComPtr<IUIAutomationElement> item; selectedItems->GetElement(0, &item);
                display = name(item.Get());
            }
        }
        if (display.empty()) return false;
        // Without a safely enumerable combo list, preserve the existing filter.
        request.filters.push_back({display, filterPattern(display)});
        request.index = 1;
        return true;
    }
    bool submit(const Result &result, const JobShared &job) {
        if (!windowMatches(job) || result.paths.size() != 1) return false;
        if (request.showReadOnly && result.readOnly != request.readOnly) {
            auto toggle = pattern<IUIAutomationTogglePattern>(readOnly.Get(), UIA_TogglePatternId);
            if (!toggle || FAILED(toggle->Toggle())) return false;
        }
        if (nativeTypes && result.index != request.index) {
            DWORD_PTR answer = 0;
            if (!result.index || result.index > request.filters.size() ||
                !send(typeWindow, CB_SETCURSEL, result.index - 1, 0, answer) || answer == (DWORD_PTR)CB_ERR) return false;
            HWND parent = GetParent(typeWindow);
            if (!parent || !PostMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(typeWindow), CBN_SELCHANGE), (LPARAM)typeWindow)) return false;
        }
        bool set = false;
        if (editWindow) {
            DWORD_PTR answer = 0;
            set = send(editWindow, WM_SETTEXT, 0, (LPARAM)result.paths.front().c_str(), answer) && answer != 0;
            if (set) set = text(editWindow) == result.paths.front();
        } else {
            auto p = pattern<IUIAutomationValuePattern>(edit.Get(), UIA_ValuePatternId);
            BSTR filename = SysAllocString(result.paths.front().c_str());
            if (p && filename) set = SUCCEEDED(p->SetValue(filename));
            SysFreeString(filename);
            if (set) set = value(edit.Get()) == result.paths.front();
        }
        if (!set || !windowMatches(job)) return false;
        if (okWindow) {
            if (!IsWindowEnabled(okWindow)) return false;
            // WM_COMMAND contains no cross-process pointers. Native processing
            // owns validation, overwrite prompts, callbacks, and the result buffer.
            return PostMessageW(GetParent(okWindow), WM_COMMAND,
                MAKEWPARAM(GetDlgCtrlID(okWindow), BN_CLICKED), (LPARAM)okWindow) != FALSE;
        }
        auto invoke = pattern<IUIAutomationInvokePattern>(ok.Get(), UIA_InvokePatternId);
        return invoke && SUCCEEDED(invoke->Invoke());
    }
    bool dismiss(const JobShared &job) {
        if (!windowMatches(job)) return true;
        if (cancelWindow) return PostMessageW(GetParent(cancelWindow), WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(cancelWindow), BN_CLICKED), (LPARAM)cancelWindow) != FALSE;
        auto invoke = pattern<IUIAutomationInvokePattern>(cancel.Get(), UIA_InvokePatternId);
        return invoke && SUCCEEDED(invoke->Invoke());
    }
};
struct Worker {
    JobShared *job;
    HANDLE cancellation = nullptr;
    Shared *session = nullptr;
    Handle controller;
    bool interrupted = false;
    explicit Worker(JobShared *j) : job(j), controller(OpenProcess(SYNCHRONIZE, FALSE, j->controllerPid)) {}
    void pulse() {
        InterlockedExchange(&job->heartbeat, (LONG)GetTickCount());
        InterlockedExchange64(&job->cancelHandle, (LONG64)(UINT_PTR)cancellation);
        if (session && session->helperPid && !job->helperPid) {
            Handle helper(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, session->helperPid));
            auto creation = helper.value ? created(helper.value) : 0;
            if (creation) {
                InterlockedExchange64(&job->helperCreation, (LONG64)creation);
                InterlockedExchange(&job->helperPid, (LONG)session->helperPid);
            }
        }
        if (job->stop || !controller.value || WaitForSingleObject(controller.value, 0) == WAIT_OBJECT_0 || !windowMatches(*job)) {
            interrupted = true;
            if (cancellation) SetEvent(cancellation);
        }
        // An application can show its dialog again during picker startup. Keep
        // only the claimed dialog hidden until native submission begins; native
        // confirmations and fallback must remain free to display afterwards.
        if (!interrupted && job->hidden && (Phase)job->phase == Phase::Picker &&
            IsWindowVisible((HWND)(UINT_PTR)job->window))
            ShowWindowAsync((HWND)(UINT_PTR)job->window, SW_HIDE);
    }
    void restore() {
        if (InterlockedExchange(&job->hidden, 0) && windowMatches(*job)) ShowWindowAsync((HWND)(UINT_PTR)job->window, SW_SHOW);
    }
    ~Worker() {
        restore();
        if (controller.value && WaitForSingleObject(controller.value, 0) == WAIT_OBJECT_0 && windowMatches(*job))
            RemovePropW((HWND)(UINT_PTR)job->window, ClaimProperty);
    }
};
LRESULT CALLBACK workerProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto worker = (Worker *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        worker = (Worker *)((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)worker);
    }
    if (message == WM_TIMER && worker) { worker->pulse(); return 0; }
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace

const wchar_t *reasonLabel(Reason reason) {
    switch (reason) {
    case Reason::WindowClass: return L"Unrecognized dialog class";
    case Reason::Accessibility: return L"Accessibility provider unavailable";
    case Reason::AccessibilityTree: return L"Accessibility tree unavailable or too large";
    case Reason::CustomOptions: return L"Custom dialog options require native UI";
    case Reason::FileView: return L"File view not found";
    case Reason::Filename: return L"Filename editor not found";
    case Reason::FileType: return L"File type selector not found";
    case Reason::Actions: return L"Open/Save or Cancel control not found";
    case Reason::ReadOnly: return L"Read-only checkbox inaccessible";
    case Reason::Mode: return L"Open/Save action label unrecognized";
    case Reason::FilenamePattern: return L"Filename editor cannot be written";
    case Reason::ActionPattern: return L"Dialog action cannot be invoked";
    case Reason::Folder: return L"Initial folder unavailable";
    case Reason::Filters: return L"File type list unavailable";
    case Reason::Identity: return L"Dialog identity or process eligibility changed";
    case Reason::WorkerWindow: return L"Worker message window unavailable";
    case Reason::Picker: return L"FilePilot picker failed";
    case Reason::Submission: return L"Native selection or cancellation delivery failed";
    case Reason::Stopped: return L"Interception stopped";
    case Reason::Exception: return L"Worker exception";
    default: return L"";
    }
}

bool windowMatches(const JobShared &job) {
    auto hwnd = (HWND)(UINT_PTR)job.window;
    DWORD pid = 0;
    if (!IsWindow(hwnd) || GetWindowThreadProcessId(hwnd, &pid) != job.targetThread || pid != job.targetPid ||
        (DWORD)(UINT_PTR)GetPropW(hwnd, ClaimProperty) != job.workerPid) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return process.value && created(process.value) == job.creation;
}
bool eligibleProcess(DWORD pid, ULONGLONG &creation, std::wstring &path) {
    if (!pid || pid == GetCurrentProcessId()) return false;
    DWORD targetSession = 0, ownSession = 0;
    if (!ProcessIdToSessionId(pid, &targetSession) || !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) || targetSession != ownSession) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value) return false;
    wchar_t image[agent::PathCapacity]; DWORD length = agent::PathCapacity;
    if (!QueryFullProcessImageNameW(process.value, 0, image, &length)) return false;
    path.assign(image, length);
    // Brokered WinRT pickers can belong to a Windows picker host rather than
    // the requesting app. Permit external dialog automation for the exact
    // system binaries, retaining the user/session/integrity checks below.
    // This exception never enables process attachment or code injection.
    wchar_t system[agent::PathCapacity]{};
    UINT systemLength = GetSystemDirectoryW(system, agent::PathCapacity);
    bool pickerHost = systemLength && systemLength < agent::PathCapacity &&
        (!_wcsicmp(path.c_str(), (std::wstring(system) + L"\\PickerHost.exe").c_str()) ||
         !_wcsicmp(path.c_str(), (std::wstring(system) + L"\\RuntimeBroker.exe").c_str()));
    if ((!pickerHost && agent::builtinExcluded(path)) || !_wcsicmp(leaf(path).c_str(), L"FPilotDialogExternal.exe")) return false;
    creation = created(process.value);
    if (!creation) return false;
    Handle targetToken, ownToken;
    if (!OpenProcessToken(process.value, TOKEN_QUERY, &targetToken.value) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownToken.value)) return false;
    auto targetUser = tokenData(targetToken.value, TokenUser), ownUser = tokenData(ownToken.value, TokenUser);
    if (targetUser.empty() || ownUser.empty() || !EqualSid(((TOKEN_USER *)targetUser.data())->User.Sid, ((TOKEN_USER *)ownUser.data())->User.Sid)) return false;
    DWORD appContainer = 0, bytes = sizeof(appContainer);
    if (!GetTokenInformation(targetToken.value, TokenIsAppContainer, &appContainer, sizeof(appContainer), &bytes) ||
        (appContainer && !pickerHost)) return false;
    DWORD targetIntegrity = integrity(targetToken.value), ownIntegrity = integrity(ownToken.value);
    DWORD minimumIntegrity = pickerHost ? SECURITY_MANDATORY_LOW_RID : SECURITY_MANDATORY_MEDIUM_RID;
    return targetIntegrity >= minimumIntegrity &&
        targetIntegrity <= ownIntegrity;
}

int runWorker(const std::wstring &mappingName) {
    Handle mapping(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName.c_str()));
    if (!mapping.value) return ERROR_INVALID_HANDLE;
    auto job = (JobShared *)MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(JobShared));
    if (!job) return ERROR_INVALID_HANDLE;
    int outcome = ERROR_NOT_SUPPORTED;
    if (job->magic != JobMagic || job->bytes != sizeof(JobShared) || job->workerPid != GetCurrentProcessId()) {
        UnmapViewOfFile(job); return ERROR_INVALID_DATA;
    }
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        diagnostic(*job, Reason::Accessibility, (DWORD)initialized);
        UnmapViewOfFile(job); return (int)initialized;
    }
    {
        Worker worker(job);
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"FPilotExternalWorker.v1"; cls.lpfnWndProc = workerProc;
        RegisterClassW(&cls);
        HWND timerWindow = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, &worker);
        if (!timerWindow) diagnostic(*job, Reason::WorkerWindow, GetLastError());
        if (timerWindow) SetTimer(timerWindow, 1, 100, nullptr);
        try {
            ULONGLONG creation = 0; std::wstring path;
            auto native = (HWND)(UINT_PTR)job->window;
            std::unique_ptr<NativeDialog> dialog;
            DWORD untilReady = GetTickCount() + 2500;
            while (timerWindow && windowMatches(*job) && !job->stop &&
                   (LONG)(untilReady - GetTickCount()) > 0) {
                worker.pulse();
                auto next = std::make_unique<NativeDialog>();
                if (next->inspect(native, *job)) { dialog = std::move(next); break; }
                MsgWaitForMultipleObjectsEx(0, nullptr, 150, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            }
            if (timerWindow && windowMatches(*job) && eligibleProcess(job->targetPid, creation, path) &&
                creation == job->creation && !job->stop && IsWindowVisible(native) && IsWindowEnabled(native) &&
                dialog) {
                worker.pulse();
                Result result;
                diagnostic(*job, Reason::Picker);
                InterlockedExchange(&job->phase, (LONG)Phase::Picker);
                // Hide before starting FilePilot, rather than waiting for its
                // initial type event. The controller can restore this window if
                // startup fails or the isolated worker stops responding.
                InterlockedExchange(&job->hidden, 1);
                ShowWindowAsync(native, SW_HIDE);
                // Owned windows can follow their owner's visibility. FilePilot
                // belongs to the requesting application, not the hidden dialog.
                auto pickerOwner = GetWindow(native, GW_OWNER);
                if (!IsWindow(pickerOwner)) pickerOwner = nullptr;
                bool pickerPresented = false;
                HRESULT hr = runPicker(job->picker, dialog->request, pickerOwner,
                    [&](Event event, Result &selection, std::wstring &message) -> HRESULT {
                        worker.pulse();
                        if (worker.interrupted) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                        if (event == Event::Type && !pickerPresented) {
                            auto pickerWindow = worker.session ? (HWND)(UINT_PTR)worker.session->helperWindow : nullptr;
                            if (pickerWindow) {
                                SetWindowPos(pickerWindow, HWND_TOP, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                                SetForegroundWindow(pickerWindow);
                                pickerPresented = true;
                            }
                        }
                        if (event == Event::Accept) return validate(dialog->request, selection, message);
                        return S_OK;
                    }, result, &worker.cancellation, nullptr, &worker.session, false);
                InterlockedExchange64(&job->cancelHandle, 0);
                worker.pulse();
                if (worker.interrupted || hr == E_NOTIMPL || (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
                    diagnostic(*job, worker.interrupted ? (job->stop ? Reason::Stopped : Reason::Identity) : Reason::Picker,
                               FAILED(hr) ? (DWORD)hr : 0);
                    InterlockedExchange(&job->phase, (LONG)(job->stop ? Phase::Stopped : Phase::Native));
                    outcome = ERROR_NOT_SUPPORTED;
                } else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                    outcome = dialog->dismiss(*job) ? 0 : ERROR_NOT_SUPPORTED;
                    diagnostic(*job, outcome ? Reason::Submission : Reason::None);
                    InterlockedExchange(&job->phase, (LONG)Phase::Cancelled);
                } else if (SUCCEEDED(hr)) {
                    InterlockedExchange(&job->phase, (LONG)Phase::Submitting);
                    diagnostic(*job, Reason::Submission);
                    if (dialog->submit(result, *job)) {
                        // The native window may remain open for an application
                        // veto or spawn an overwrite prompt. Reveal it in either
                        // case, instead of pretending the save was completed.
                        DWORD until = GetTickCount() + 800;
                        while (windowMatches(*job) && (LONG)(until - GetTickCount()) > 0) {
                            worker.pulse();
                            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                            MSG m;
                            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                        }
                        InterlockedExchange(&job->phase, (LONG)Phase::Submitted);
                        diagnostic(*job, Reason::None);
                        outcome = 0;
                    } else {
                        InterlockedExchange(&job->phase, (LONG)Phase::Native);
                        outcome = ERROR_NOT_SUPPORTED;
                    }
                }
                worker.restore();
                if (windowMatches(*job)) SetForegroundWindow(native);
            } else {
                if (job->stop) diagnostic(*job, Reason::Stopped);
                else if (timerWindow && (dialog || !windowMatches(*job))) diagnostic(*job, Reason::Identity);
                InterlockedExchange(&job->phase, (LONG)(job->stop ? Phase::Stopped : Phase::Native));
            }
        } catch (...) {
            diagnostic(*job, Reason::Exception);
            InterlockedExchange(&job->phase, (LONG)Phase::Native);
            outcome = ERROR_NOT_SUPPORTED;
        }
        if (timerWindow) DestroyWindow(timerWindow);
    }
    CoUninitialize();
    UnmapViewOfFile(job);
    return outcome;
}
} // namespace fpp::external
