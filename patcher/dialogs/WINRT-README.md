# FilePilot WinRT Open/Save provider

This additional provider handles `Windows.Storage.Pickers.FileOpenPicker` and
`Windows.Storage.Pickers.FileSavePicker`. It coexists with the installed modern
COM replacement and external fallback. It has no API detours, remote memory
writes, process attachment, resident injector, or application launcher.
Windows activation loads our x64/x86 DLL normally into the requesting process.
Custom code therefore executes in that process.

## Build and install

`build-winrt.ps1` compiles only the two production provider DLLs and packages
the existing COM payload's FilePilot picker and UI runtime. No tests or runtime
validations are written or run. It publishes `winrt-runtime.txt` independently
of `com-runtime.txt`; existing COM registration and payloads are unchanged.

From an elevated **64-bit** PowerShell:

```powershell
& .\install-winrt.ps1 -Action Install
```

Installation copies an immutable `WinRtRuntime-1-*` payload into
`C:\Program Files\Voidstar\FilePilotDialogs`, then changes only `DllPath` in
the existing machine activation registrations for the two picker classes, in
both registry views. This is a system-class registration replacement, not a
documented third-party file-picker extension point. It does not edit Windows
DLL files, activation type, trust level, threading, owner, or registry ACLs.
Temporary backup/restore privileges provide access to the protected keys and
are returned to their previous state when the installer exits.

The installer requires the original provider to be the architecture-appropriate
Windows `twinui.appcore.dll`, with in-process activation. Foreign providers are
not overwritten. Raw original `DllPath` bytes and value kind are journaled under
`HKLM\Software\Voidstar\FilePilotDialogs\WinRtRegistration.v1` before each write.
Pending and installed paths are recorded before/after publication; an interrupted
installation can be restored with `-Action Uninstall`. Restoration preserves
subsequent foreign registration changes and retains the journal on conflict.
Payloads are retained because applications can still have their DLLs loaded.

Fully close and reopen applications after installation to release cached WinRT
factories. The installer does not close applications or open any file dialogs.

```powershell
& .\install-winrt.ps1 -Action Disable
& .\install-winrt.ps1 -Action Enable
& .\install-winrt.ps1 -Action Uninstall
```

Enable/Disable affect the current user's `WinRtEnabled` setting; a disabled
provider delegates to Windows. If COM mode is installed, its `ComEnabled` setting
also gates WinRT replacement. The existing exclusions file applies. Machine
registration affects all users, but replacement is off for users who have not
enabled it. Uninstall needs elevation and restores the original machine paths;
other users' cached objects remain alive until their applications exit.
An installer copy is also kept at
`C:\Program Files\Voidstar\FilePilotDialogs\install-winrt.ps1` for recovery.

## Supported behavior

The exported `DllGetActivationFactory` obtains the original factory directly
from the architecture-appropriate System32 `twinui.appcore.dll`. This avoids
recursive activation through the replaced registrations. Windows' module is
kept loaded for the process lifetime, including private interfaces and fallback
objects passed back to callers.

The native object supplies property validation, file-type collections, optional
interfaces, and unsupported operations. Our wrapper intercepts ordinary
`PickSingleFileAsync`, `PickMultipleFilesAsync`, and `PickSaveFileAsync` calls
when a full-trust application has initialized the picker with a valid owner HWND
from its own process. Supported operations never call the native display method.
The existing FilePilot helper receives the filename, location, button label and
file types. The suggested save file takes priority over remembered locations.
Locations are remembered per picker kind and SettingsIdentifier.

The result is a genuine Windows `StorageFile`, or a vector view of StorageFiles
for multiple selection. Save probes write access with `OPEN_ALWAYS`, creating
only a missing file and preserving existing file content. Existing destinations
require overwrite confirmation. Selection/access failures remain in FilePilot
so users can correct the selection. Ordinary picker cancellation returns a null
file or empty vector; `IAsyncInfo.Cancel` signals a request-owned cancellation
event and the C++/WinRT operation follows its async cancellation contract.
Work runs on a COM-initialized background MTA; caller UI threads remain free to
pump messages. Factories, objects, async operations and background work all hold
the provider module alive.

## Deliberate native delegation and remaining coverage

AppContainer callers retain Windows' native factory: selecting a path in an
external helper does not grant the app the brokered file-access permission that
the Windows picker supplies. Other delegated cases include excluded processes,
disabled replacement, missing helper, uninitialized/foreign owner HWND,
explicit-user factory activation, enterprise-protected saves, non-filesystem
suggested files, operation-ID/resume and continuation contracts, and private
interfaces. These requests may still use external fallback when recognized,
and that fallback can flash.

`Microsoft.Windows.Storage.Pickers` from the Windows App SDK is a different
namespace/provider and is not registered by this installer. Legacy
`GetOpenFileName`/`GetSaveFileName` exports are unchanged. This addition therefore
does not establish universal suppression. Loader/signature policy and activation
caching may affect whether applications load a replacement system-class provider;
no functional coverage claim has been established by runtime trials.

## Sources and design basis

- [WinRT factory resolution](https://learn.microsoft.com/en-us/windows/win32/api/roapi/nf-roapi-rogetactivationfactory)
- [Windows runtime factory/DLL construction](https://devblogs.microsoft.com/cppblog/ccx-part-3-of-n-under-construction/)
- [FileSavePicker API and StorageFile results](https://learn.microsoft.com/en-us/uwp/api/windows.storage.pickers.filesavepicker)
- [Desktop window initialization](https://learn.microsoft.com/en-us/windows/apps/develop/ui/display-ui-objects)
- [Asynchronous C++/WinRT cancellation](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/concurrency-2)
- [Brokered file-access permissions](https://learn.microsoft.com/en-us/windows/apps/develop/files/file-access-permissions)
- [Registry backup/restore access](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regcreatekeyexw)
