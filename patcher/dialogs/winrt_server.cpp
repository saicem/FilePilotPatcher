// Windows.Storage.Pickers activation bridge. No detours or process attachment.
#include "client.h"
#include "agent_shared.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <roapi.h>
#include <activation.h>
#include <winstring.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <memory>
#include <mutex>

namespace wf = winrt::Windows::Foundation;
namespace wc = winrt::Windows::Foundation::Collections;
namespace ws = winrt::Windows::Storage;
namespace wp = winrt::Windows::Storage::Pickers;
using winrt::hstring;

namespace {
HINSTANCE moduleInstance{};
std::once_flag nativeOnce;
HMODULE nativeModule{};
using GetFactory = HRESULT (WINAPI *)(HSTRING, ::IActivationFactory **);
GetFactory nativeFactory{};
DWORD nativeError = ERROR_MOD_NOT_FOUND;
constexpr wchar_t OpenClass[] = L"Windows.Storage.Pickers.FileOpenPicker";
constexpr wchar_t SaveClass[] = L"Windows.Storage.Pickers.FileSavePicker";

struct ModuleLease {
    ModuleLease() { ++winrt::get_module_lock(); }
    ~ModuleLease() { --winrt::get_module_lock(); }
};
std::wstring text(hstring const& value) { return {value.c_str(), value.size()}; }

void loadNative() {
    wchar_t system[32768];
    auto count = GetSystemDirectoryW(system, ARRAYSIZE(system));
    if (!count || count >= ARRAYSIZE(system)) {
        nativeError = count ? ERROR_INSUFFICIENT_BUFFER : GetLastError(); return;
    }
    // Never resolve the original provider through the registrations we override.
    auto path = std::wstring(system, count) + L"\\twinui.appcore.dll";
    nativeModule = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nativeModule) { nativeError = GetLastError(); return; }
    nativeFactory = reinterpret_cast<GetFactory>(GetProcAddress(nativeModule, "DllGetActivationFactory"));
    nativeError = nativeFactory ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND;
    // Native interfaces may escape through private QI or fallback. Keep their
    // Windows vtables resident independently of this bridge's unload lifetime.
}
wf::IActivationFactory originalFactory(hstring const& name) {
    std::call_once(nativeOnce, loadNative);
    if (!nativeFactory) winrt::throw_hresult(HRESULT_FROM_WIN32(nativeError));
    wf::IActivationFactory factory{nullptr};
    winrt::check_hresult(nativeFactory(reinterpret_cast<HSTRING>(winrt::get_abi(name)),
        reinterpret_cast<::IActivationFactory **>(winrt::put_abi(factory))));
    return factory;
}
std::wstring helperPath() {
    wchar_t file[32768];
    auto count = GetModuleFileNameW(moduleInstance, file, ARRAYSIZE(file));
    if (!count || count >= ARRAYSIZE(file)) return {};
    return fpp::parent(std::wstring(file, count)) + L"\\FPilotPicker.exe";
}
bool setting(const wchar_t *name) {
    DWORD value{}, bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, fpp::agent::RegistryPath, name,
        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value, &bytes) == ERROR_SUCCESS && value;
}
bool enabled() {
    if (!setting(L"WinRtEnabled")) return false;
    if (setting(L"ComModeInstalled") && !setting(L"ComEnabled")) return false;
    winrt::handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) return false;
    DWORD appContainer{}, bytes{};
    if (!GetTokenInformation(token.get(), TokenIsAppContainer, &appContainer,
                             sizeof(appContainer), &bytes) || appContainer) return false;
    wchar_t image[32768]; DWORD count = ARRAYSIZE(image);
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, image, &count) ||
        fpp::agent::builtinExcluded(std::wstring(image, count))) return false;
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return false;
    auto path = std::wstring(local) + L"\\Voidstar\\FilePilotDialogs\\exclusions.txt";
    CoTaskMemFree(local);
    winrt::handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > 65536) return false;
    std::string data(static_cast<size_t>(size.QuadPart), '\0'); DWORD read{};
    if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) ||
        read != data.size()) return false;
    if (data.compare(0, 3, "\xef\xbb\xbf") == 0) data.erase(0, 3);
    auto chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(),
                                    static_cast<int>(data.size()), nullptr, 0);
    if (!chars) return data.empty();
    std::wstring patterns(chars, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(),
                       static_cast<int>(data.size()), patterns.data(), chars);
    return !fpp::agent::excluded(std::wstring(image, count), patterns);
}
std::wstring startFolder(wp::PickerLocationId location) {
    const KNOWNFOLDERID *id = &FOLDERID_Documents;
    switch (location) {
    case wp::PickerLocationId::Desktop: id = &FOLDERID_Desktop; break;
    case wp::PickerLocationId::Downloads: id = &FOLDERID_Downloads; break;
    case wp::PickerLocationId::PicturesLibrary: id = &FOLDERID_Pictures; break;
    case wp::PickerLocationId::MusicLibrary: id = &FOLDERID_Music; break;
    case wp::PickerLocationId::VideosLibrary: id = &FOLDERID_Videos; break;
    case wp::PickerLocationId::ComputerFolder: return L"";
    default: break;
    }
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(*id, 0, nullptr, &path))) return {};
    std::wstring value(path); CoTaskMemFree(path); return value;
}
std::wstring locationKey(bool save, hstring const& identifier) {
    // Use a value name, not a subkey, so the application's identifier is opaque.
    return std::wstring(save ? L"Save." : L"Open.") + text(identifier).substr(0, 256);
}
std::wstring rememberedFolder(std::wstring const& key) {
    wchar_t path[32768]; DWORD bytes = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Voidstar\\FilePilotDialogs\\WinRtLocations",
        key.c_str(), RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &bytes) != ERROR_SUCCESS)
        return {};
    return fpp::directory(path) ? path : L"";
}
void rememberFolder(std::wstring const& key, std::wstring const& folder) {
    HKEY root{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Voidstar\\FilePilotDialogs\\WinRtLocations",
        0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &root, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(root, key.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(folder.c_str()),
                      static_cast<DWORD>((folder.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(root);
    }
}
std::wstring pattern(wc::IVector<hstring> const& extensions) {
    std::wstring out;
    for (auto const& value : extensions) {
        if (!out.empty()) out += L';';
        auto ext = text(value);
        out += ext == L"*" ? L"*.*" : L"*" + ext;
    }
    return out;
}
bool canRoute(HWND owner) {
    DWORD pid{};
    auto helper = helperPath();
    return enabled() && owner && IsWindow(owner) &&
        GetWindowThreadProcessId(owner, &pid) && pid == GetCurrentProcessId() &&
        GetFileAttributesW(helper.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct Work {
    ModuleLease lease;
    winrt::handle cancellation{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::shared_ptr<std::atomic<bool>> busy;
    std::atomic<bool> finished{false};
    explicit Work(std::shared_ptr<std::atomic<bool>> value) : busy(std::move(value)) {
        winrt::check_bool(static_cast<bool>(cancellation));
        if (busy->exchange(true)) winrt::throw_hresult(E_ILLEGAL_METHOD_CALL);
    }
    void finish() { if (!finished.exchange(true)) busy->store(false); }
    ~Work() { finish(); }
};
struct Completion {
    std::shared_ptr<Work> work;
    ~Completion() { work->finish(); }
};
struct ApartmentLease {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ApartmentLease() { winrt::check_hresult(result); }
    ~ApartmentLease() { CoUninitialize(); }
};
std::wstring errorMessage(HRESULT hr) {
    PWSTR buffer{};
    auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0,
        reinterpret_cast<PWSTR>(&buffer), 0, nullptr);
    std::wstring value = count ? std::wstring(buffer, count) : L"Windows could not access this file.";
    if (buffer) LocalFree(buffer);
    return value;
}

// Runs on a COM-initialized MTA. Construct genuine Windows StorageFile objects
// while the picker remains open, so file-access failures can be corrected there.
std::vector<ws::StorageFile> choose(fpp::Request const& request, HWND owner,
                                  std::shared_ptr<Work> const& work, std::wstring const& key) {
    ApartmentLease apartment;
    fpp::Result result;
    HWND helper{};
    std::vector<ws::StorageFile> files;
    auto callback = [&](fpp::Event event, fpp::Result &selection, std::wstring &message) -> HRESULT {
        if (WaitForSingleObject(work->cancellation.get(), 0) == WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (event != fpp::Event::Accept) return S_OK;
        auto hr = fpp::validate(request, selection, message);
        if (hr != S_OK) return hr;
        if (request.mode == fpp::Mode::Save && !selection.paths.empty() &&
            GetFileAttributesW(selection.paths.front().c_str()) != INVALID_FILE_ATTRIBUTES &&
            MessageBoxW(helper, (L"Replace the existing file?\n\n" + selection.paths.front()).c_str(),
                        L"Save As", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            message = L"Choose another filename.";
            return S_FALSE;
        }
        try {
            std::vector<ws::StorageFile> selected;
            for (auto const& path : selection.paths) {
                if (request.mode == fpp::Mode::Save) {
                    // OPEN_ALWAYS probes write access and creates only a missing
                    // file. Never truncate the application's existing document.
                    winrt::handle file{CreateFileW(path.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
                    if (!file) winrt::throw_last_error();
                }
                selected.push_back(ws::StorageFile::GetFileFromPathAsync(path).get());
            }
            files = std::move(selected);
            return S_OK;
        } catch (...) {
            message = errorMessage(winrt::to_hresult());
            return S_FALSE;
        }
    };
    auto hr = fpp::runPicker(helperPath(), request, owner, callback, result,
                            nullptr, &helper, nullptr, true, work->cancellation.get());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    winrt::check_hresult(hr);
    if (!result.paths.empty()) rememberFolder(key, fpp::parent(result.paths.front()));
    return files;
}
wf::IAsyncOperation<ws::StorageFile> pickOne(fpp::Request request, HWND owner,
                                          std::shared_ptr<Work> work, std::wstring key) {
    Completion completion{work};
    auto cancellation = co_await winrt::get_cancellation_token();
    cancellation.callback([work] { SetEvent(work->cancellation.get()); });
    co_await winrt::resume_background();
    auto files = choose(request, owner, work, key);
    co_return files.empty() ? ws::StorageFile{nullptr} : files.front();
}
wf::IAsyncOperation<wc::IVectorView<ws::StorageFile>> pickMany(fpp::Request request, HWND owner,
                                                           std::shared_ptr<Work> work, std::wstring key) {
    Completion completion{work};
    auto cancellation = co_await winrt::get_cancellation_token();
    cancellation.callback([work] { SetEvent(work->cancellation.get()); });
    co_await winrt::resume_background();
    auto files = choose(request, owner, work, key);
    co_return winrt::single_threaded_vector<ws::StorageFile>(std::move(files)).GetView();
}

// Native property collections retain Windows' extension validation and ordering.
// The native display methods are called only on explicitly delegated requests.
struct OpenPicker : winrt::implements<OpenPicker, wp::IFileOpenPicker,
    wp::IFileOpenPicker2, wp::IFileOpenPicker3, wp::IFileOpenPickerWithOperationId,
    ::IInitializeWithWindow, winrt::non_agile> {
    wp::FileOpenPicker native;
    HWND owner{};
    std::shared_ptr<std::atomic<bool>> busy = std::make_shared<std::atomic<bool>>(false);
    explicit OpenPicker(wp::FileOpenPicker value) : native(std::move(value)) {}
    hstring GetRuntimeClassName() const { return OpenClass; }
    int32_t query_interface_tearoff(winrt::guid const& id, void **out) const noexcept override {
        return winrt::get_unknown(native)->QueryInterface(id, out);
    }
    HRESULT STDMETHODCALLTYPE Initialize(HWND window) noexcept override {
        try {
            auto hr = native.as<::IInitializeWithWindow>()->Initialize(window);
            if (SUCCEEDED(hr)) owner = window;
            return hr;
        } catch (...) { return winrt::to_hresult(); }
    }
    auto ViewMode() { return native.ViewMode(); }
    void ViewMode(wp::PickerViewMode value) { native.ViewMode(value); }
    auto SettingsIdentifier() { return native.SettingsIdentifier(); }
    void SettingsIdentifier(hstring const& value) { native.SettingsIdentifier(value); }
    auto SuggestedStartLocation() { return native.SuggestedStartLocation(); }
    void SuggestedStartLocation(wp::PickerLocationId value) { native.SuggestedStartLocation(value); }
    auto CommitButtonText() { return native.CommitButtonText(); }
    void CommitButtonText(hstring const& value) { native.CommitButtonText(value); }
    auto FileTypeFilter() { return native.FileTypeFilter(); }
    auto ContinuationData() { return native.ContinuationData(); }
    void PickSingleFileAndContinue() { native.PickSingleFileAndContinue(); }
    void PickMultipleFilesAndContinue() { native.PickMultipleFilesAndContinue(); }
    auto User() { return native.User(); }
    fpp::Request request(bool many) {
        fpp::Request value;
        value.mode = fpp::Mode::Open;
        value.options |= FOS_FORCEFILESYSTEM | (many ? FOS_ALLOWMULTISELECT : 0);
        value.title = L"Open";
        value.okLabel = text(native.CommitButtonText());
        auto key = locationKey(false, native.SettingsIdentifier());
        value.folder = rememberedFolder(key);
        if (value.folder.empty()) value.folder = startFolder(native.SuggestedStartLocation());
        auto types = native.FileTypeFilter();
        if (!types.Size()) winrt::throw_hresult(E_INVALIDARG);
        value.filters.push_back({L"Supported files", pattern(types)});
        return value;
    }
    wf::IAsyncOperation<ws::StorageFile> PickSingleFileAsync() {
        if (!canRoute(owner)) return native.PickSingleFileAsync();
        auto value = request(false);
        return pickOne(std::move(value), owner, std::make_shared<Work>(busy),
                       locationKey(false, native.SettingsIdentifier()));
    }
    wf::IAsyncOperation<wc::IVectorView<ws::StorageFile>> PickMultipleFilesAsync() {
        if (!canRoute(owner)) return native.PickMultipleFilesAsync();
        auto value = request(true);
        return pickMany(std::move(value), owner, std::make_shared<Work>(busy),
                        locationKey(false, native.SettingsIdentifier()));
    }
    wf::IAsyncOperation<ws::StorageFile> PickSingleFileAsync(hstring const& operationId) {
        // Resume/operation-ID contracts require the original Windows broker.
        return native.PickSingleFileAsync(operationId);
    }
};
struct SavePicker : winrt::implements<SavePicker, wp::IFileSavePicker,
    wp::IFileSavePicker2, wp::IFileSavePicker3, wp::IFileSavePicker4,
    ::IInitializeWithWindow, winrt::non_agile> {
    wp::FileSavePicker native;
    HWND owner{};
    std::shared_ptr<std::atomic<bool>> busy = std::make_shared<std::atomic<bool>>(false);
    explicit SavePicker(wp::FileSavePicker value) : native(std::move(value)) {}
    hstring GetRuntimeClassName() const { return SaveClass; }
    int32_t query_interface_tearoff(winrt::guid const& id, void **out) const noexcept override {
        return winrt::get_unknown(native)->QueryInterface(id, out);
    }
    HRESULT STDMETHODCALLTYPE Initialize(HWND window) noexcept override {
        try {
            auto hr = native.as<::IInitializeWithWindow>()->Initialize(window);
            if (SUCCEEDED(hr)) owner = window;
            return hr;
        } catch (...) { return winrt::to_hresult(); }
    }
    auto SettingsIdentifier() { return native.SettingsIdentifier(); }
    void SettingsIdentifier(hstring const& value) { native.SettingsIdentifier(value); }
    auto SuggestedStartLocation() { return native.SuggestedStartLocation(); }
    void SuggestedStartLocation(wp::PickerLocationId value) { native.SuggestedStartLocation(value); }
    auto CommitButtonText() { return native.CommitButtonText(); }
    void CommitButtonText(hstring const& value) { native.CommitButtonText(value); }
    auto FileTypeChoices() { return native.FileTypeChoices(); }
    auto DefaultFileExtension() { return native.DefaultFileExtension(); }
    void DefaultFileExtension(hstring const& value) { native.DefaultFileExtension(value); }
    auto SuggestedSaveFile() { return native.SuggestedSaveFile(); }
    void SuggestedSaveFile(ws::StorageFile const& value) { native.SuggestedSaveFile(value); }
    auto SuggestedFileName() { return native.SuggestedFileName(); }
    void SuggestedFileName(hstring const& value) { native.SuggestedFileName(value); }
    auto ContinuationData() { return native.ContinuationData(); }
    void PickSaveFileAndContinue() { native.PickSaveFileAndContinue(); }
    auto EnterpriseId() { return native.EnterpriseId(); }
    void EnterpriseId(hstring const& value) { native.EnterpriseId(value); }
    auto User() { return native.User(); }
    wf::IAsyncOperation<ws::StorageFile> PickSaveFileAsync() {
        if (!canRoute(owner) || !native.EnterpriseId().empty()) return native.PickSaveFileAsync();
        fpp::Request value;
        value.mode = fpp::Mode::Save;
        value.options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                        FOS_NOREADONLYRETURN | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES;
        value.title = L"Save As";
        value.filename = text(native.SuggestedFileName());
        value.okLabel = text(native.CommitButtonText());
        auto key = locationKey(true, native.SettingsIdentifier());
        value.folder = rememberedFolder(key);
        if (value.folder.empty()) value.folder = startFolder(native.SuggestedStartLocation());
        auto suggested = native.SuggestedSaveFile();
        if (suggested) {
            auto path = text(suggested.Path());
            if (path.empty()) return native.PickSaveFileAsync();
            value.filename = fpp::leaf(path); value.folder = fpp::parent(path);
        }
        auto choices = native.FileTypeChoices();
        if (!choices.Size()) winrt::throw_hresult(E_INVALIDARG);
        for (auto const& choice : choices)
            value.filters.push_back({text(choice.Key()), pattern(choice.Value())});
        value.extension = text(native.DefaultFileExtension());
        if (value.extension.empty()) {
            auto first = choices.First().Current().Value();
            if (first.Size()) value.extension = text(first.GetAt(0));
        }
        if (!value.extension.empty() && value.extension.front() == L'.') value.extension.erase(0, 1);
        return pickOne(std::move(value), owner, std::make_shared<Work>(busy), std::move(key));
    }
};

// A statics interface is first so these are ordinary allocated wrappers rather
// than C++/WinRT's default-construction-only, cached activation-factory type.
struct OpenFactory : winrt::implements<OpenFactory, wp::IFileOpenPickerStatics,
    wf::IActivationFactory, wp::IFileOpenPickerStatics2, winrt::non_agile> {
    wf::IActivationFactory native;
    explicit OpenFactory(wf::IActivationFactory value) : native(std::move(value)) {}
    hstring GetRuntimeClassName() const { return OpenClass; }
    int32_t query_interface_tearoff(winrt::guid const& id, void **out) const noexcept override {
        return winrt::get_unknown(native)->QueryInterface(id, out);
    }
    wf::IInspectable ActivateInstance() {
        auto object = native.ActivateInstance<wf::IInspectable>();
        if (!enabled()) return object;
        return winrt::make<OpenPicker>(object.as<wp::FileOpenPicker>());
    }
    auto ResumePickSingleFileAsync() { return native.as<wp::IFileOpenPickerStatics>().ResumePickSingleFileAsync(); }
    auto CreateForUser(winrt::Windows::System::User const& user) {
        // Explicit-user activation retains Windows' identity and access contract.
        return native.as<wp::IFileOpenPickerStatics2>().CreateForUser(user);
    }
};
struct SaveFactory : winrt::implements<SaveFactory, wp::IFileSavePickerStatics,
    wf::IActivationFactory, winrt::non_agile> {
    wf::IActivationFactory native;
    explicit SaveFactory(wf::IActivationFactory value) : native(std::move(value)) {}
    hstring GetRuntimeClassName() const { return SaveClass; }
    int32_t query_interface_tearoff(winrt::guid const& id, void **out) const noexcept override {
        return winrt::get_unknown(native)->QueryInterface(id, out);
    }
    wf::IInspectable ActivateInstance() {
        auto object = native.ActivateInstance<wf::IInspectable>();
        if (!enabled()) return object;
        return winrt::make<SavePicker>(object.as<wp::FileSavePicker>());
    }
    auto CreateForUser(winrt::Windows::System::User const& user) {
        return native.as<wp::IFileSavePickerStatics>().CreateForUser(user);
    }
};
} // namespace

extern "C" HRESULT WINAPI DllGetActivationFactory(HSTRING classId, ::IActivationFactory **out) {
    ModuleLease lease;
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        UINT32 length{};
        auto chars = WindowsGetStringRawBuffer(classId, &length);
        hstring name(chars, length);
        if (name != OpenClass && name != SaveClass) return CLASS_E_CLASSNOTAVAILABLE;
        auto native = originalFactory(name);
        wf::IActivationFactory factory{nullptr};
        if (!enabled()) factory = native;
        else if (name == OpenClass) factory = winrt::make_self<OpenFactory>(std::move(native)).as<wf::IActivationFactory>();
        else factory = winrt::make_self<SaveFactory>(std::move(native)).as<wf::IActivationFactory>();
        *out = reinterpret_cast<::IActivationFactory *>(winrt::detach_abi(factory));
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) moduleInstance = instance;
    return TRUE;
}
