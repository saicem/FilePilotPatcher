#include "native_ui.h"
#include "protocol.h"
#include "vendor/minhook/include/MinHook.h"
#include <atomic>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <intrin.h>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winternl.h>

using namespace fpp;
namespace {
Handles session;
Request request;
BYTE *image;
UINT64 app;
HWND root, browser;
NativePickerUi pickerUi;
bool readySeen = false;
std::wstring caption;
std::wstring effectiveCommand, privateFolder, lastFolder, lastSelection;
std::atomic<DWORD> enumerationIndex{1};
bool busy = false, finished = false, testRan = false;
DWORD uiThread;
using OpenSelection = void(__fastcall *)(void *, BYTE *, int *);
using SidebarOpen = void(__fastcall *)(void *, size_t *, int);
using Frame = void(__fastcall *)(UINT64);
OpenSelection originalOpen;
SidebarOpen originalSidebar;
Frame originalFrame;
using Describe = BYTE *(__fastcall *)(void *, BYTE *);
Describe describe;
decltype(&CreateWindowExW) nativeCreateWindow;
decltype(&SHGetKnownFolderPath) nativeKnownFolder;
decltype(&GetProcAddress) nativeGetProc;
decltype(&DispatchMessageW) nativeDispatch;
using NtQuery = NTSTATUS(NTAPI *)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID,
                                  ULONG, FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
NtQuery nativeQuery;
constexpr UINT Pulse = WM_APP + 19, AcceptPending = WM_APP + 21,
               CancelPending = WM_APP + 22, TypePending = WM_APP + 23, RefreshPending = WM_APP + 24;
using UiBegin = void(__fastcall *)(DWORD, UINT64, UINT64, UINT64);
using UiEnd = void(__fastcall *)();
using UiRender = void(__fastcall *)(BYTE *, DWORD, int, int);
using PanelRectangle = RECT *(__fastcall *)(RECT *, UINT64, UINT64);
using SidebarUi = void(__fastcall *)(UINT64, UINT64, UINT64, UINT64);
UiBegin originalUiBegin;
UiEnd originalUiEnd;
UiEnd originalUiFinish;
UiRender originalUiRender;
BYTE *footerPaintRoot = nullptr;
unsigned uiRenderDepth = 0;
bool footerPainted = false;
PanelRectangle originalPanelRectangle;
SidebarUi originalSidebarUi;
void __fastcall onUiBegin(DWORD layer, UINT64 b, UINT64 c, UINT64 d) {
    // Only the first root in the outer frame is the browser. Other callers
    // create noninteractive measurement roots, including the sidebar's.
    auto caller = (BYTE *)_ReturnAddress();
    auto ui = *(BYTE **)(image + 0x249278);
    bool mainRoot =
        caller == image + 0x1a3375 && layer == 0 && *(unsigned short *)(ui + 0x282) == 0;
    int fullHeight = *(int *)(ui + 12);
    if (mainRoot && app && *(void **)(app + 0xa58)) {
        pickerUi.setApp(app);
        pickerUi.prepareInput();
        *(int *)(ui + 12) = fullHeight - pickerUi.reservedHeight(fullHeight);
    }
    originalUiBegin(layer, b, c, d);
    if (mainRoot) {
        *(int *)(ui + 12) = fullHeight;
        auto nativeRoot = *(BYTE **)(ui + 0xf8);
        *(UINT64 *)(nativeRoot + 0x20) |= 0x200000; // Browser clipping includes its hit area.
    }
}
RECT *__fastcall onPanelRectangle(RECT *out, UINT64 a, UINT64 panel) {
    // Panels use explicit geometry computed before the UI tree is built.
    // Their rectangle source must reserve the footer too; a wrapper cannot
    // change those absolute coordinates.
    pickerUi.setApp(a);
    auto renderer = *(BYTE **)(image + 0x249288);
    auto &height = *(int *)(renderer + 0x48);
    int fullHeight = height;
    height -= pickerUi.reservedHeight(fullHeight);
    auto result = originalPanelRectangle(out, a, panel);
    height = fullHeight;
    return result;
}
void __fastcall onSidebarUi(UINT64 a, UINT64 b, UINT64 c, UINT64 d) {
    // 1accd0 builds the resize strip, its separate visible line and the sidebar
    // viewport from renderer+48 on pass zero. Shortening only the strip at
    // paint time does not change the already laid-out line or viewport.
    // Reserve the footer at their shared geometry source. Popup placement
    // still uses the full UI-context height; menu overlays keep that space.
    pickerUi.setApp(a);
    auto renderer = *(BYTE **)(image + 0x249288);
    auto &height = *(int *)(renderer + 0x48);
    int fullHeight = height;
    height -= pickerUi.reservedHeight(fullHeight);
    originalSidebarUi(a, b, c, d);
    height = fullHeight;
}
void __fastcall onUiFinish() {
    if (app && *(void **)(app + 0xa58)) {
        pickerUi.setApp(app);
        footerPaintRoot = pickerUi.render(originalUiBegin, originalUiEnd);
        footerPainted = false;
    }
    originalUiFinish();
    footerPaintRoot = nullptr;
}
void __fastcall onUiRender(BYTE *widget, DWORD layer, int a, int b) {
    if (footerPaintRoot && uiRenderDepth == 0) {
        // Context menus are children of the primary browser root, not
        // additional root slots. Paint the footer before any native tree so
        // those menus can extend over it. Retain native root indices, layout,
        // cache cleanup and finalization; skip only the later footer draw.
        if (widget == footerPaintRoot && footerPainted)
            return;
        if (!footerPainted && widget != footerPaintRoot) {
            footerPainted = true;
            ++uiRenderDepth;
            originalUiRender(footerPaintRoot, 0, 0, 0);
            --uiRenderDepth;
        } else if (widget == footerPaintRoot)
            footerPainted = true;
    }
    ++uiRenderDepth;
    originalUiRender(widget, layer, a, b);
    --uiRenderDepth;
    pickerUi.recordNameGeometry(widget);
}
constexpr UINT Timer = 0xf119;
void trace(const char *message) {
    wchar_t file[32768];
    if (GetEnvironmentVariableW(L"FPP_TRACE", file, 32768)) {
        std::ofstream stream(std::filesystem::path(file), std::ios::app);
        stream << GetCurrentProcessId() << " " << message << std::endl;
    }
}
LONG CALLBACK exceptionTrace(EXCEPTION_POINTERS *p) {
    char line[256];
    sprintf_s(line, "exception %08lx at %llx (image rva %llx), rcx %llx rdx %llx",
              p->ExceptionRecord->ExceptionCode, (UINT64)p->ExceptionRecord->ExceptionAddress,
              (UINT64)p->ExceptionRecord->ExceptionAddress - (UINT64)image, p->ContextRecord->Rcx,
              p->ContextRecord->Rdx);
    trace(line);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::wstring utf8(const char *text, size_t n) {
    if (!text || n > 32767 * 4)
        return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)n, nullptr, 0);
    if (!count)
        return {};
    std::wstring s(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)n, s.data(), count);
    return s;
}
void setName(const std::wstring &value) {
    if (!pickerUi.setName(value))
        pickerUi.setStatus(L"The filename is too long for FilePilot's text editor.");
}
void status(const std::wstring &value) {
    pickerUi.setStatus(value);
}
BYTE *panel() {
    return app ? *(BYTE **)(app + 0xaf8) : nullptr;
}
std::wstring folder() {
    auto p = panel();
    if (!p)
        return request.folder;
    auto model = *(BYTE **)(p + 0x90);
    if (!model)
        return request.folder;
    return utf8(*(const char **)(model + 0x168), *(size_t *)(model + 0x170));
}
std::wstring path(BYTE *p, BYTE *item) {
    if (!p || !item)
        return {};
    auto d = describe(*(void **)(p + 0x90), item);
    return d ? utf8(*(const char **)(d + 0x10), *(size_t *)(d + 0x18)) : L"";
}
std::vector<std::wstring> selected(BYTE *p) {
    std::vector<std::wstring> out;
    if (!p)
        return out;
    if (*(DWORD *)(p + 0x120) == 0) {
        auto s = path(p, *(BYTE **)(p + 0x128));
        if (!s.empty())
            out.push_back(s);
    } else {
        auto count = *(size_t *)(p + 0x178);
        auto rows = *(BYTE **)(p + 0x1b0);
        if (count > 128)
            return {};
        for (size_t i = 0; rows && i < count; i++) {
            auto s = path(p, *(BYTE **)(rows + i * 0x10));
            if (!s.empty())
                out.push_back(s);
        }
    }
    return out;
}
void close() {
    if (finished)
        return;
    finished = true;
    if (root)
        ShowWindow(root, SW_HIDE);
    ExitProcess(0);
}
void cancel() {
    if (finished)
        return;
    SetEvent(session.cancel);
    close();
}
HRESULT notify(Event event, const Result &result) {
    if (busy || finished)
        return E_PENDING;
    auto data = encode(result);
    busy = true;
    ResetEvent(session.decision);
    memcpy(session.shared->event, data.data(), data.size());
    session.shared->eventBytes = (DWORD)data.size();
    session.shared->kind = (LONG)event;
    MemoryBarrier();
    SetEvent(session.event);
    HRESULT hr = E_FAIL;
    for (;;) {
        HANDLE handles[] = {session.decision, session.cancel};
        DWORD wait = MsgWaitForMultipleObjectsEx(2, handles, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            MemoryBarrier();
            hr = session.shared->reply;
            try {
                Reader r(session.shared->replyText, session.shared->replyBytes);
                status(r.string());
                r.end();
            } catch (...) {
                hr = E_FAIL;
            }
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1)
            cancel();
        if (wait == WAIT_OBJECT_0 + 2) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT)
                    cancel();
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, session.shared->ownerPid);
        if (!owner)
            cancel();
        bool dead = WaitForSingleObject(owner, 0) == WAIT_OBJECT_0;
        CloseHandle(owner);
        if (dead)
            cancel();
    }
    busy = false;
    return hr;
}
DWORD filterIndex() {
    return pickerUi.type();
}
bool matches(const std::wstring &name, DWORD index) {
    if (request.filters.empty())
        return true;
    auto pattern =
        request.filters[std::min<size_t>(index ? index - 1 : 0, request.filters.size() - 1)]
            .pattern;
    size_t start = 0;
    while (start <= pattern.size()) {
        auto end = pattern.find(L';', start);
        auto part =
            pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
        if (part == L"*" || part == L"*.*" || PathMatchSpecW(name.c_str(), part.c_str()))
            return true;
        if (end == pattern.npos)
            break;
        start = end + 1;
    }
    return false;
}
std::wstring extension(DWORD index) {
    if (request.filters.empty())
        return request.extension;
    auto pat = request.filters[std::min<size_t>(index ? index - 1 : 0, request.filters.size() - 1)]
                   .pattern;
    auto end = pat.find(L';');
    pat = pat.substr(0, end);
    if (pat.rfind(L"*.", 0) == 0 && pat.find_first_of(L"*?", 2) == pat.npos)
        return pat.substr(2);
    return request.extension;
}
void navigate(const std::wstring &value) {
    if (!app || !directory(value))
        return;
    int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0, nullptr,
                                nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), s.data(), n, nullptr,
                        nullptr);
    size_t view[] = {(size_t)s.data(), s.size()};
    originalSidebar((void *)app, view, 1);
}
void accept(std::vector<std::wstring> paths = {}) {
    if (busy || finished)
        return;
    try {
        auto dir = folder();
        if (dir.empty())
            dir = request.folder;
        DWORD index = filterIndex();
        if (paths.empty()) {
            auto typed = pickerUi.name();
            if (request.mode == Mode::Folder) {
                if (!typed.empty())
                    paths.push_back(full(typed, dir));
                else {
                    auto selection = selected(panel());
                    paths.push_back(selection.size() == 1 && directory(selection.front())
                                        ? selection.front()
                                        : dir);
                }
            } else if (!typed.empty())
                paths.push_back(full(typed, dir));
            else
                paths = selected(panel());
        }
        if (paths.empty()) {
            status(L"Choose a file or enter a filename.");
            return;
        }
        if (request.mode != Mode::Open || !(request.options & FOS_ALLOWMULTISELECT))
            paths.resize(1);
        for (auto &value : paths) {
            value = full(value, dir);
            if (directory(value) && request.mode != Mode::Folder) {
                std::string s;
                int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr,
                                            0, nullptr, nullptr);
                s.resize(n);
                WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), s.data(), n,
                                    nullptr, nullptr);
                size_t view[] = {(size_t)s.data(), s.size()};
                originalSidebar((void *)app, view, 1);
                setName(L"");
                return;
            }
            if (request.mode == Mode::Save) {
                auto file = leaf(value);
                auto ext = extension(index);
                if (file.find_last_of(L'.') == file.npos && !ext.empty())
                    value += L"." + ext;
            }
            auto attributes = GetFileAttributesW(value.c_str());
            if (request.mode == Mode::Folder && !directory(value)) {
                status(L"Choose a folder.");
                return;
            }
            if ((request.options & FOS_PATHMUSTEXIST) && !directory(parent(value))) {
                status(L"The destination folder does not exist.");
                return;
            }
            if ((request.options & FOS_FILEMUSTEXIST) && attributes == INVALID_FILE_ATTRIBUTES) {
                status(L"The file does not exist.");
                return;
            }
            if ((request.options & FOS_NOREADONLYRETURN) && attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_READONLY)) {
                status(L"The file is read-only.");
                return;
            }
            if ((request.options & FOS_STRICTFILETYPES) && !matches(leaf(value), index)) {
                status(L"The filename does not match the selected file type.");
                return;
            }
        }
        if (notify(Event::Accept, {paths, dir, index, pickerUi.readOnly()}) == S_OK)
            close();
        else if (!request.testPaths.empty())
            testRan = false;
    } catch (...) {
        status(L"Cannot prepare this file selection.");
    }
}
void __fastcall onOpen(void *a, BYTE *p, int *options) {
    try {
        auto paths = selected(p);
        if (paths.empty())
            return;
        bool allDirectories =
            std::all_of(paths.begin(), paths.end(), [](auto &s) { return directory(s); });
        if (allDirectories) {
            originalOpen(a, p, options);
            return;
        }
        if (request.mode == Mode::Save) {
            setName(leaf(paths.front()));
            pickerUi.focusName(true);
            return;
        }
        if (request.mode != Mode::Folder)
            accept(paths);
    } catch (...) {
        status(L"Cannot read the selected file.");
    }
}
void __fastcall onSidebar(void *a, size_t *view, int) {
    auto s = utf8((const char *)view[0], view[1]);
    if (directory(s)) {
        originalSidebar(a, view, 1);
        return;
    }
    if (request.mode == Mode::Save) {
        setName(leaf(s));
        pickerUi.focusName(true);
        return;
    }
    if (request.mode == Mode::Open)
        accept({s});
}
void __fastcall onFrame(UINT64 a) {
    app = a;
    originalFrame(a);
    if (root)
        PostMessageW(root, Pulse, 0, 0);
}
LPWSTR WINAPI commandLine() {
    return effectiveCommand.data();
}
HRESULT WINAPI knownFolder(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR *out) {
    if (id != FOLDERID_LocalAppData && id != FOLDERID_RoamingAppData)
        return nativeKnownFolder(id, flags, token, out);
    *out = (PWSTR)CoTaskMemAlloc((privateFolder.size() + 1) * 2);
    if (!*out)
        return E_OUTOFMEMORY;
    memcpy(*out, privateFolder.c_str(), (privateFolder.size() + 1) * 2);
    return S_OK;
}
HANDLE WINAPI createMutex(LPSECURITY_ATTRIBUTES security, BOOL owner, LPCWSTR name) {
    auto isolated =
        name ? std::wstring(name) + L"_Picker_" + std::to_wstring(GetCurrentProcessId()) : L"";
    return CreateMutexW(security, owner, name ? isolated.c_str() : nullptr);
}
LSTATUS WINAPI denyRegSet(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD) {
    return ERROR_ACCESS_DENIED;
}
LSTATUS WINAPI denyRegDelete(HKEY, LPCWSTR) {
    return ERROR_ACCESS_DENIED;
}
LSTATUS WINAPI denyRegCreate(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                             const LPSECURITY_ATTRIBUTES, PHKEY out, LPDWORD) {
    if (out)
        *out = nullptr;
    return ERROR_ACCESS_DENIED;
}
NTSTATUS NTAPI query(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID context,
                     PIO_STATUS_BLOCK io, PVOID buffer, ULONG bytes, FILE_INFORMATION_CLASS kind,
                     BOOLEAN single, PUNICODE_STRING mask, BOOLEAN restart) {
    if (request.filters.empty() || event || apc)
        return nativeQuery(handle, event, apc, context, io, buffer, bytes, kind, single, mask,
                           restart);
    unsigned nameOffset = 0, attributes = 56, length = 60;
    switch ((int)kind) {
    case 1:
        nameOffset = 64;
        break;
    case 2:
        nameOffset = 68;
        break;
    case 3:
        nameOffset = 94;
        break;
    case 37:
        nameOffset = 104;
        break;
    case 38:
        nameOffset = 80;
        break;
    default:
        return nativeQuery(handle, event, apc, context, io, buffer, bytes, kind, single, mask,
                           restart);
    }
    for (;;) {
        auto result = nativeQuery(handle, event, apc, context, io, buffer, bytes, kind, single,
                                  mask, restart);
        restart = FALSE;
        if (result != 0 || io->Information > bytes)
            return result;
        size_t input = 0, output = 0, previous = SIZE_MAX;
        auto data = (BYTE *)buffer;
        while (input < io->Information) {
            if (input + nameOffset > io->Information)
                return result;
            DWORD next, n, flags;
            memcpy(&next, data + input, 4);
            memcpy(&n, data + input + length, 4);
            memcpy(&flags, data + input + attributes, 4);
            if (n % 2 || input + nameOffset + n > io->Information)
                return result;
            size_t span = next ? next : io->Information - input;
            if (span < nameOffset + n || input + span > io->Information)
                return result;
            std::wstring value(n / 2, L'\0');
            memcpy(value.data(), data + input + nameOffset, n);
            if ((flags & FILE_ATTRIBUTE_DIRECTORY) || matches(value, enumerationIndex.load())) {
                memmove(data + output, data + input, span);
                if (previous != SIZE_MAX)
                    *(DWORD *)(data + previous) = (DWORD)(output - previous);
                previous = output;
                output += span;
            }
            if (!next)
                break;
            input += next;
        }
        if (output) {
            *(DWORD *)(data + previous) = 0;
            io->Information = output;
            return result;
        }
        if (io->Information == 0)
            return result;
    }
}
FARPROC WINAPI getProc(HMODULE module, LPCSTR name) {
    auto p = nativeGetProc(module, name);
    if ((ULONG_PTR)name > 65535 && strcmp(name, "NtQueryDirectoryFile") == 0) {
        nativeQuery = (NtQuery)p;
        return (FARPROC)query;
    }
    return p;
}

LRESULT WINAPI dispatch(const MSG *message) {
    return nativeDispatch(message);
}
LRESULT CALLBACK rootProc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == ControlMessage) {
        try {
            MemoryBarrier();
            auto update = decodeRequest(session.shared->control, session.shared->controlBytes);
            if (w & Control::Name)
                setName(update.filename);
            if (w & Control::Title) {
                caption = update.title;
                SetWindowTextW(root, caption.c_str());
            }
            if (w & Control::OkLabel)
                pickerUi.setOkLabel(update.okLabel);
            if (w & Control::TypeIndex) {
                pickerUi.setType(update.index);
                enumerationIndex = filterIndex();
                PostMessageW(root, RefreshPending, 0, 0);
            }
            if (w & Control::Extension)
                request.extension = update.extension;
            if (w & Control::Folder)
                navigate(update.folder);
            update.filename = pickerUi.name();
            update.folder = folder();
            update.index = filterIndex();
            auto data = encode(update);
            memcpy(session.shared->control, data.data(), data.size());
            session.shared->controlBytes = (DWORD)data.size();
            MemoryBarrier();
            return 1;
        } catch (...) {
            return 0;
        }
    }
    if (message == WM_SETTEXT)
        return DefSubclassProc(hwnd, message, w, (LPARAM)caption.c_str());
    if (message == WM_GETMINMAXINFO) {
        auto result = DefSubclassProc(hwnd, message, w, l);
        auto limits = (MINMAXINFO *)l;
        auto dpi = (int)GetDpiForWindow(hwnd);
        limits->ptMinTrackSize.x = std::max<LONG>(limits->ptMinTrackSize.x, MulDiv(680, dpi, 96));
        limits->ptMinTrackSize.y = std::max<LONG>(limits->ptMinTrackSize.y, MulDiv(480, dpi, 96));
        return result;
    }
    if (message == RefreshPending) {
        // Deliver the browser refresh directly to FilePilot, including when
        // the file-type selector currently has keyboard focus.
        DefSubclassProc(hwnd, WM_KEYDOWN, VK_F5, 0);
        DefSubclassProc(hwnd, WM_KEYUP, VK_F5, 0);
        return 0;
    }
    if (message == WM_CLOSE || message == CancelPending) {
        cancel();
        return 0;
    }
    if (message == AcceptPending) {
        accept();
        return 0;
    }
    if (message == TypePending) {
        if (!busy) {
            request.index = filterIndex();
            enumerationIndex = request.index;
            notify(Event::Type, {{}, folder(), request.index});
            PostMessageW(root, RefreshPending, 0, 0);
        }
        return 0;
    }
    if (message == Pulse || (message == WM_TIMER && w == Timer)) {
        if (busy || finished)
            return 0;
        if (WaitForSingleObject(session.cancel, 0) == WAIT_OBJECT_0)
            cancel();
        if (!app || !*(void **)(app + 0xa58))
            return 0;
        try {
            if (!readySeen && IsWindowVisible(root)) {
                readySeen = true;
                SetForegroundWindow(root);
                SetFocus(root);
                MemoryBarrier();
                SetEvent(session.ready);
                // Native dialogs report their initial type once the window is
                // available. Browser sinks use this event to initialize owner
                // tracking and acceptance timing, even without a type change.
                notify(Event::Type, {{}, folder(), filterIndex()});
            }
            auto dir = folder();
            if (!dir.empty() && dir != lastFolder) {
                auto previous = lastFolder;
                auto response = notify(Event::Folder, {{}, dir, filterIndex()});
                if (response != S_OK && response != E_NOTIMPL) {
                    navigate(previous.empty() ? request.folder : previous);
                    return 0;
                }
                lastFolder = dir;
                if (request.mode != Mode::Save)
                    setName(L"");
            }
            auto paths = selected(panel());
            std::wstring key;
            for (auto &s : paths)
                key += s + L"\n";
            if (key != lastSelection) {
                lastSelection = key;
                if (!pickerUi.editing()) {
                    if (request.mode != Mode::Folder && paths.size() == 1 &&
                        !directory(paths.front()))
                        setName(leaf(paths.front()));
                    else if (request.mode == Mode::Open)
                        setName(L"");
                }
                notify(Event::Selection, {paths, dir, filterIndex()});
            }
            if (!testRan && (request.testCancel || !request.testPaths.empty())) {
                testRan = true;
                if (request.testCancel)
                    cancel();
                else
                    accept(request.testPaths);
            }
        } catch (...) {
            status(L"Cannot update the selection.");
        }
        return 0;
    }
    if (pickerUi.message(message, w, l))
        return 0;
    return DefSubclassProc(hwnd, message, w, l);
}
HWND WINAPI createWindow(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int width,
                         int height, HWND parent, HMENU menu, HINSTANCE instance,
                         LPVOID parameter) {
    if (parent || !title || wcscmp(title, L"File Pilot") != 0)
        return nativeCreateWindow(ex, cls, title, style, x, y, width, height, parent, menu,
                                  instance, parameter);
    uiThread = GetCurrentThreadId();
    // Preserve FilePilot's top-level window and custom chrome. There is no
    // second host HWND, SetParent transition, or Win32 footer control.
    root = browser = nativeCreateWindow(ex, cls, caption.c_str(), style, x, y, std::max(width, 680),
                                        std::max(height, 480), parent, menu, instance, parameter);
    if (!root)
        return nullptr;
    if (!SetWindowSubclass(root, rootProc, 0xf119, 0)) {
        DestroyWindow(root);
        root = browser = nullptr;
        return nullptr;
    }
    if (session.shared->ownerWindow)
        SetWindowLongPtrW(root, GWLP_HWNDPARENT, (LONG_PTR)session.shared->ownerWindow);
    session.shared->helperPid = GetCurrentProcessId();
    session.shared->helperWindow = (UINT64)root;
    SetTimer(root, Timer, 80, nullptr);
    return root;
}
bool iat(const char *name, void *replacement, void **original = nullptr) {
    auto nt = (IMAGE_NT_HEADERS64 *)(image + ((IMAGE_DOS_HEADER *)image)->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto dll = (IMAGE_IMPORT_DESCRIPTOR *)(image + dir.VirtualAddress);
    for (; dll->Name; ++dll) {
        auto names = (IMAGE_THUNK_DATA64 *)(image + dll->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA64 *)(image + dll->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            auto entry = (IMAGE_IMPORT_BY_NAME *)(image + names->u1.AddressOfData);
            if (strcmp((char *)entry->Name, name))
                continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old))
                return false;
            if (original)
                *original = (void *)slots->u1.Function;
            slots->u1.Function = (UINT64)replacement;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            return true;
        }
    }
    return false;
}
bool hook(size_t rva, void *replacement, void **original) {
    return MH_CreateHook(image + rva, replacement, original) == MH_OK;
}
} // namespace

extern "C" __declspec(dllexport) BOOL WINAPI PickerInitialize(BYTE *base) {
    try {
        trace("initialize entered");
        wchar_t tracePath[32768];
        if (GetEnvironmentVariableW(L"FPP_TRACE", tracePath, 32768))
            AddVectoredExceptionHandler(1, exceptionTrace);
        image = base;
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args || count != 7 || wcscmp(args[1], L"--fp-picker-v1")) {
            if (args)
                LocalFree(args);
            return FALSE;
        }
        HANDLE *handles[] = {&session.mapping, &session.ready, &session.event, &session.decision,
                             &session.cancel};
        for (int i = 0; i < 5; i++)
            *handles[i] = (HANDLE)_wcstoui64(args[i + 2], nullptr, 16);
        LocalFree(args);
        session.shared =
            (Shared *)MapViewOfFile(session.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
        if (!session.shared || session.shared->magic != Magic || session.shared->version != Version)
            return FALSE;
        request = decodeRequest(session.shared->request, session.shared->requestBytes);
        caption = request.title.empty()
                      ? (request.mode == Mode::Save     ? L"Save As ? FilePilot"
                         : request.mode == Mode::Folder ? L"Choose Folder ? FilePilot"
                                                        : L"Open ? FilePilot")
                      : request.title;
        auto ok = request.okLabel.empty() ? (request.mode == Mode::Save     ? L"Save"
                                             : request.mode == Mode::Folder ? L"Select Folder"
                                                                            : L"Open")
                                          : request.okLabel;
        std::vector<std::wstring> types;
        for (auto &filter : request.filters)
            types.push_back(filter.label + L" (" + filter.pattern + L")");
        pickerUi.initialize(image, &root,
                            {[] {
                                 if (root)
                                     PostMessageW(root, AcceptPending, 0, 0);
                             },
                             [] {
                                 if (root)
                                     PostMessageW(root, CancelPending, 0, 0);
                             },
                             [](DWORD) {
                                 if (root)
                                     PostMessageW(root, TypePending, 0, 0);
                             }});
        pickerUi.configure(request.filename, ok, types, request.index, request.showReadOnly,
                           request.readOnly, request.mode == Mode::Save);
        enumerationIndex = pickerUi.type();
        trace("request decoded");
        if (!directory(request.folder)) {
            wchar_t cwd[32768];
            GetCurrentDirectoryW(32768, cwd);
            request.folder = cwd;
        }
        wchar_t exe[32768];
        GetModuleFileNameW(nullptr, exe, 32768);
        effectiveCommand = quote(exe) + L" " + quote(request.folder);
        PWSTR data = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &data)))
            return FALSE;
        privateFolder = std::wstring(data) + L"\\Voidstar\\FilePilotPicker\\Sessions\\" +
                        std::to_wstring(GetCurrentProcessId());
        CoTaskMemFree(data);
        SHCreateDirectoryExW(nullptr, privateFolder.c_str(), nullptr);
        describe = (Describe)(image + 0x67400);
        trace("installing native hooks");
        if (MH_Initialize() != MH_OK || !hook(0x5e090, (void *)onOpen, (void **)&originalOpen) ||
            !hook(0x1c870, (void *)onSidebar, (void **)&originalSidebar) ||
            !hook(0x167ed0, (void *)onFrame, (void **)&originalFrame) ||
            !hook(0x1ce3e0, (void *)onUiBegin, (void **)&originalUiBegin) ||
            !hook(0x116720, (void *)onPanelRectangle, (void **)&originalPanelRectangle) ||
            !hook(0x1accd0, (void *)onSidebarUi, (void **)&originalSidebarUi) ||
            !hook(0x1d5080, (void *)onUiRender, (void **)&originalUiRender) ||
            !hook(0x1cc6c0, (void *)onUiFinish, (void **)&originalUiFinish))
            return FALSE;
        originalUiEnd = (UiEnd)(image + 0x1cebd0);
        trace("installing imports");
        if (!iat("GetCommandLineW", (void *)commandLine) ||
            !iat("CreateWindowExW", (void *)createWindow, (void **)&nativeCreateWindow) ||
            !iat("SHGetKnownFolderPath", (void *)knownFolder, (void **)&nativeKnownFolder))
            return FALSE;
        // Native context-menu verbs and file operations must reach Windows.
        // Selection activation is adapted at onOpen/onSidebar, rather than by
        // denying CreateFile/MoveFile/IFileOperation or hijacking ShellExecute.
        iat("RegSetValueExW", (void *)denyRegSet);
        iat("GetProcAddress", (void *)getProc, (void **)&nativeGetProc);
        iat("CreateMutexW", (void *)createMutex);
        iat("RegDeleteTreeW", (void *)denyRegDelete);
        iat("RegDeleteValueW", (void *)denyRegDelete);
        iat("RegCreateKeyExW", (void *)denyRegCreate);
        iat("DispatchMessageW", (void *)dispatch, (void **)&nativeDispatch);
        auto result = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
        trace(result ? "initialized" : "hook enabling failed");
        return result;
    } catch (...) {
        trace("initialization exception");
        return FALSE;
    }
}
