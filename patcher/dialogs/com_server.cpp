#include "modern_dialog.h"
#include <atomic>
#include <mutex>
#include <shlobj.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace {
HINSTANCE moduleInstance = nullptr;
std::atomic<LONG> objects{0}, serverLocks{0}, activeCalls{0};
std::once_flag nativeOnce;
HMODULE nativeModule = nullptr;
using GetFactory = HRESULT (STDAPICALLTYPE *)(REFCLSID, REFIID, void **);
GetFactory getNativeFactory = nullptr;
DWORD nativeError = ERROR_MOD_NOT_FOUND;

struct CallLease {
    CallLease() { ++activeCalls; }
    ~CallLease() { --activeCalls; }
};
void retainModule() { ++objects; }
void releaseModule() { --objects; }

void loadNative() {
    wchar_t folder[32768];
    UINT length = GetSystemDirectoryW(folder, ARRAYSIZE(folder));
    if (!length || length >= ARRAYSIZE(folder)) {
        nativeError = length ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        return;
    }
    // WOW64 resolves this to the matching architecture. Never read the native
    // server path from HKCR: that registration now points back at this module.
    auto path = std::wstring(folder, length) + L"\\comdlg32.dll";
    nativeModule = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nativeModule) { nativeError = GetLastError(); return; }
    getNativeFactory = reinterpret_cast<GetFactory>(GetProcAddress(nativeModule, "DllGetClassObject"));
    nativeError = getNativeFactory ? ERROR_SUCCESS : GetLastError();
    // Keep Windows' native module loaded for the process lifetime, including
    // native objects returned for private IIDs or aggregation. No loader-lock
    // teardown and no dangling vtables when this bridge is unloaded.
}

std::wstring pickerPath() {
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(moduleInstance, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return {};
    std::wstring value(path, length);
    return value.substr(0, value.find_last_of(L"\\/")) + L"\\FPilotPicker.exe";
}

bool enabled() {
    DWORD installed = 0, value = 0, bytes = sizeof(DWORD);
    auto flags = RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY;
    if (RegGetValueW(HKEY_CURRENT_USER, fpp::agent::RegistryPath, L"ComModeInstalled",
                    flags, nullptr, &installed, &bytes) != ERROR_SUCCESS || !installed) return false;
    bytes = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, fpp::agent::RegistryPath, L"ComEnabled",
                    flags, nullptr, &value, &bytes) != ERROR_SUCCESS || !value) return false;
    wchar_t image[32768]; DWORD length = ARRAYSIZE(image);
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, image, &length)) return false;
    if (fpp::agent::builtinExcluded(image)) return false;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return false;
    auto filePath = std::wstring(local) + L"\\Voidstar\\FilePilotDialogs\\exclusions.txt";
    CoTaskMemFree(local);
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 65536) {
        CloseHandle(file); return false;
    }
    std::string data(static_cast<size_t>(size.QuadPart), '\0'); DWORD read = 0;
    bool ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok || read != data.size()) return false;
    if (data.compare(0, 3, "\xef\xbb\xbf") == 0) data.erase(0, 3);
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), static_cast<int>(data.size()), nullptr, 0);
    if (!count) return data.empty();
    std::wstring patterns(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), static_cast<int>(data.size()), patterns.data(), count);
    return !fpp::agent::excluded(image, patterns);
}

class Factory final : public IClassFactory {
    std::atomic<ULONG> references{1};
    ComPtr<IClassFactory> native;
    fpp::modern::Runtime runtime;
    bool save;
public:
    Factory(IClassFactory *factory, bool isSave) : native(factory), save(isSave) {
        runtime.pickerExecutable = pickerPath();
        runtime.enabled = enabled;
        runtime.retainModule = retainModule;
        runtime.releaseModule = releaseModule;
        retainModule();
    }
    ~Factory() { native.Reset(); releaseModule(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --references;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        try {
            // Private interfaces and aggregation retain the Windows factory's
            // original behavior instead of inventing an incomplete COM object.
            if (outer || !fpp::modern::knownInterface(iid, save) || !enabled())
                return native->CreateInstance(outer, iid, out);
            ComPtr<IUnknown> object;
            auto hr = native->CreateInstance(nullptr, IID_PPV_ARGS(&object));
            if (FAILED(hr)) return hr;
            return fpp::modern::wrap(object.Get(), save, iid, out, runtime);
        } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            ++serverLocks;
            auto hr = native->LockServer(TRUE);
            if (FAILED(hr)) --serverLocks;
            return hr;
        }
        LONG count = serverLocks.load();
        do { if (count <= 0) return E_UNEXPECTED; }
        while (!serverLocks.compare_exchange_weak(count, count - 1));
        auto hr = native->LockServer(FALSE);
        if (FAILED(hr)) ++serverLocks;
        return hr;
    }
};
} // namespace

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID iid, void **out) {
    CallLease lease;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        std::call_once(nativeOnce, loadNative);
        if (!getNativeFactory) return HRESULT_FROM_WIN32(nativeError);
        ComPtr<IClassFactory> native;
        auto hr = getNativeFactory(clsid, IID_PPV_ARGS(&native));
        if (FAILED(hr)) return hr;
        auto factory = new Factory(native.Get(), clsid == CLSID_FileSaveDialog);
        hr = factory->QueryInterface(iid, out);
        factory->Release();
        return hr;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    return objects.load() || serverLocks.load() || activeCalls.load() ? S_FALSE : S_OK;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) moduleInstance = instance;
    return TRUE;
}
