# COM replacement with external fallback

The user reported Paint still showing native dialogs. Static inspection of the
installed Microsoft.Paint 11.2605.81.0 package found `GetOpenFileNameW` in the
imports of `PaintApp/mspaint.exe` and the UTF-16 runtime class name
`Windows.Storage.Pickers.FileSavePicker` in `PaintApp/PaintUI.dll`. This establishes
available legacy and WinRT paths; no application-dialog trial was performed to
identify which individual action executes each path. Both can bypass the
FileOpenDialog/FileSaveDialog CLSID overrides.

### Subsequent static call tracing in Paint 11.2605.81.0

Inspection of actual instructions, rather than imports/class-name strings alone,
establishes three implemented picker routes:

| Module | Call-site evidence | Route |
| --- | --- | --- |
| mspaint.exe | RVA 0x72c76 calls the GetOpenFileNameW import from the routine at RVA 0x71f20, which builds image/project file filters | Legacy Open |
| mspaint.exe | RVA 0x70412 calls CoCreateInstance with CLSID_FileSaveDialog and IID_IFileSaveDialog from the routine at RVA 0x70390; that routine retrieves the Show vtable slot at RVA 0x717f8 | Modern COM Save |
| PaintUI.dll | RVA 0x39a566 enters the FileSavePicker activation helper at RVA 0x7bd90; the same routine queries IInitializeWithWindow at RVA 0x39ad34 and calls the IFileSavePicker PickSaveFileAsync slot (0x98) at RVA 0x39ade8 | WinRT Save |

The COM requested IID is 84BCCd23-5FDE-4CDB-AEA4-AF64B83D78AB and its CLSID is
C0B4E2F3-BA21-4773-8DBA-335EC946EB8B. The WinRT window-initialization IID is
3E68D4BD-7135-4D10-8018-9FB6D9F33FA1; SDK interface definitions identify the
Save picker slot. These findings establish executable call paths, including a
modern COM Save route not identified in the earlier import/string survey.
They do not establish which route a specific user-observed menu action takes,
or whether COM activation in that application's security context honors the
current-user registration. No file dialogs or functional trials were run.

Microsoft describes legacy file dialogs separately from the modern common item
dialog, and WinRT FileSavePicker is a separately activated Windows Runtime class:
[legacy Open/Save APIs](https://learn.microsoft.com/en-us/windows/win32/dlgbox/open-and-save-as-dialog-boxes),
[FileSavePicker](https://learn.microsoft.com/en-us/uwp/api/windows.storage.pickers.filesavepicker).

The external watcher previously refused startup while COM mode was installed.
It now complements COM mode. Supported COM calls continue to open FilePilot
before native display. A native `#32770` window that bypasses COM is considered
by the existing external recognition pipeline: filename editor, type selector,
file view, Open/Save action and Cancel must be identifiable. The watcher hides
the native dialog, runs the same FilePilot picker, transfers the path/type and
invokes the original dialog action. Native code retains validation, callbacks,
result buffers and WinRT completion. Picker windows and all FilePilot controller
processes are excluded, so this does not intercept the picker recursively.

The exact System32 PickerHost.exe and RuntimeBroker.exe paths are eligible for
external file-dialog automation even if their token is low integrity or
AppContainer. The exception is local to external recognition, never process
attachment. Same-user/session, target-integrity-at-most-watcher, process creation
identity and Windows access restrictions remain in place. Other critical,
AppContainer or low-integrity processes remain excluded. A candidate window is
converted only after semantic recognition succeeds. Protected or inaccessible
dialogs and unsupported/custom controls retain native UI.

With COM installed, external runtime resolution reads `com-runtime.txt` and uses
its immutable picker/worker payload. This prevents legacy requests from loading
an old agent payload with outdated filename UI. Standalone external mode still
uses `agent-runtime.txt`. External fallback observes ComEnabled; disabling COM
also stops new external requests and restores active hidden native windows.
Tray Pause applies to fallback only and survives payload upgrades.

The COM installer starts the watcher hidden and adds the separately owned
`FilePilot Dialog Fallback` startup entry. The recovery journal records its
exact command before publication. Upgrades stop the owned controller normally,
publish the new binary and restart it. Uninstall stops the watcher, removes only
its matching startup entry, and restores the COM registration backups. Existing
foreign startup commands and controllers are not overwritten. Worker/watchdog
recovery continues to restore native windows; caller applications are not
terminated.

This extends coverage without a launcher or FilePilot remote injection. It is
not complete replacement of every dialog: native discovery occurs after
creation, so a flash is possible, only one path is returned externally, and
unrecognized application-drawn/WinRT layouts retain their original UI.
No tests, validations or functional dialog trials are written or run, per the
user's instruction. Production compilation and installation alone do not
establish Paint's runtime routing.

Production targets were compiled and the combined package installed for both
COM architecture views on 2026-10-10:
`ComRuntime-1-20261010-064812-2947938`. The installer registered the external
fallback startup command and requested its hidden background launch.

## Additional WinRT activation provider

The subsequent request to devise and implement a WinRT adapter added
`patcher/dialogs/winrt_server.cpp`, separate x64/x86 production targets,
`build-winrt.ps1`, and the reversible `install-winrt.ps1` machine installer.
See [the provider design and operating instructions](../patcher/dialogs/WINRT-README.md).

On this machine, both Open/Save Windows.Storage.Pickers classes use in-process
activation through twinui.appcore.dll. The 64-bit registrations name
`C:\Windows\System32\twinui.appcore.dll`; the 32-bit registrations name
`C:\Windows\SysWOW64\twinui.appcore.dll`. Administrators have read access to
the protected registration keys. The installer uses temporary backup/restore
privileges to open the existing keys, changing only DllPath without editing ACLs
or ownership. It journals original raw values and installs its provider/helper
payload beneath Program Files rather than registering a user-writable DLL.

The provider gets native factories directly from the matching Windows DLL,
preserves native property/collection validation, wraps ordinary default-created
Open/Save objects and intercepts their async display calls. Supported full-trust
desktop requests with a caller-owned initialized HWND use FilePilot before the
native display method is called. Async cancellation, single/multiple Open,
suggested names/files, extension choices, remembered locations, file-access
errors and genuine StorageFile results are handled. A new save destination is
created with OPEN_ALWAYS; existing file contents are not truncated by the picker.

Sandboxed requests retain the Windows provider because ordinary path selection
does not supply brokered file-access grants. Explicit-user, continuation/resume,
operation-ID, enterprise-protected and private interface contracts also delegate.
Windows App SDK's separate Microsoft.Windows.Storage.Pickers classes and legacy
comdlg32 exports are not replaced by this installer. Native flashes remain
possible on delegated requests handled by the external watcher. No claim of
universal runtime routing or suppression is made.

Production x64/x86 DLL compilation and the actual installer completed on
2026-10-10, publishing
`C:\Program Files\Voidstar\FilePilotDialogs\WinRtRuntime-1-20261010-070937-3121022`.
Existing COM registrations and the external controller were not changed by this
installation. No tests, validation scripts or functional dialog trials were
written or run. Applications were not terminated; existing cached factories
require a full application restart before the new activation route can apply.

## Paint COM Save metadata fallback correction

After the user reported a native flash while saving, read-only module inventory
of the running Paint process (PID 29392) found FPilotDialogCom64.dll from the
previous ComRuntime-1-20261010-064812-2947938 payload loaded, alongside Windows
comdlg32.dll. The WinRT provider DLL was not in that module inventory.

Further static tracing of mspaint.exe's COM Save routine found:

- RVA 0x7179d supplies `prop:System.Security.EncryptionOwners` to the property
  description-list constructor.
- RVA 0x717d7 calls IFileSaveDialog::SetCollectedProperties(list, FALSE).
- RVA 0x71831 invokes the dialog's Show method.

Previously modern_dialog.cpp routed every SetCollectedProperties request through
saveCall, which unconditionally set NativeConfiguration::SaveMetadata. Show then
called native->Show; the external watcher could only hide the resulting window
after creation. This is a concrete code path that explains a native flash even
when Paint has successfully loaded our COM replacement.

Read-only inspection of the current Windows property schema returned type flags
0x8000001B for System.Security.EncryptionOwners, including PDTF_ISINNATE. Microsoft
defines that flag as read-only/non-writable:
[property type flags](https://learn.microsoft.com/en-us/windows/win32/api/propsys/ne-propsys-propdesc_type_flags).
Paint's FALSE append argument excludes file-type default property editors:
[SetCollectedProperties](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifilesavedialog-setcollectedproperties).

The adapter now forwards collection configuration to Windows, inspects each
description, and permits collections containing only innate values/group
headings without native display. Empty collections also remain supported.
Editable properties, appended default properties, oversized/unknown lists or
description failures retain native metadata fallback. This rule uses Windows'
description flags rather than a Paint executable or property-name exception.
For supported collections, GetProperties reads the collected values from the
selected Shell item into a memory property store; absent values for new files
stay absent. ApplyProperties remains delegated to Windows. No encryption or
metadata edits are made by the picker.

Production x64/x86 targets compiled and the actual COM installer completed for
ComRuntime-1-20261010-072107-8733759. The separate WinRT registration remains
installed. Paint was not terminated and can retain its previous bridge/factories
until it fully exits. No tests, validation scripts or functional dialog trials
were written or run; the post-restart visual behavior remains unconfirmed.
