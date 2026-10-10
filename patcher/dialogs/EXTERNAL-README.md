# External FilePilot dialog interceptor

The [COM replacement](COM-README.md) handles registered modern dialogs before
native display through normal in-process COM loading. This watcher can now run
alongside it as the fallback for recognizable native file-dialog windows that
bypass that registration. `install-com.ps1 -Action Install` installs and starts
both, retaining the no-launcher workflow.

`FPilotDialogExternal.exe` watches standard desktop Open and Save As windows and
uses the existing FilePilot picker. It does not inject DLLs into applications,
detour their APIs, alter COM registrations, or require application launchers.
Separate processes belonging to the interceptor handle accessibility calls and
picker sessions. These workers are not application injection.

Install standalone external mode for the current user's sign-in, or enable the
fallback alongside an installed COM package:

```powershell
& .\FPilotDialogAgent.exe --quit
& .\FPilotDialogExternal.exe --install
```

Standalone mode replaces this package's existing `FilePilot Dialog Agent` Run
entry with the external executable. COM fallback uses the separate `FilePilot
Dialog Fallback` entry and the COM payload. Stop the injection agent first. Its previously
loaded managed adapters become inactive when its policy shuts down; restarting
applications removes those resident adapters entirely. External mode does not
load any new ones. Do not run both interception modes together. The injection
agent refuses a new start while the external watcher is running; the external
watcher suspends new requests if an injection agent is present.

```powershell
& .\FPilotDialogExternal.exe --background  # Current session only
& .\FPilotDialogExternal.exe --status
& .\FPilotDialogExternal.exe --pause
& .\FPilotDialogExternal.exe --enable
& .\FPilotDialogExternal.exe --quit
& .\FPilotDialogExternal.exe --uninstall
```

Running without arguments opens the status window. Its tray menu provides
pause/enable, startup, exclusions, status, and Exit. The normal window Close
button hides status while interception continues. Exclusions reuse the UTF-8
`%LOCALAPPDATA%\Voidstar\FilePilotDialogs\exclusions.txt` file. Invalid exclusion
configuration suspends new requests rather than discarding exclusions.

## How it works

Out-of-context WinEvent hooks notify the watcher about window visibility and
foreground changes; a 500 ms reconciliation sweep covers missed events. The
watcher checks process user, session, and integrity before launching a worker.
A single x64 watcher handles x64 and x86 native windows through Windows messages
and UI Automation. ARM64 deployment is not included.

A worker waits briefly for dialog initialization, then requires the standard
dialog class, a file view, an identifiable filename editor and type selector,
and recognizable Open/Save and Cancel controls. Recognition uses standard control
identifiers and UI Automation properties rather than the window title alone.
Modern Windows filename/type hosts are also queried directly from native combo
handles. Their semantic `FileNameControlHost` / `FileTypeControlHost` IDs bind
the actual filename Edit and type dropdown even when accessibility reports
plain panes without patterns or omits them from the root descendant tree.
The native address toolbar supplies the current folder when omitted there too.
Unknown/custom action labels, unrecognized layouts, folder-only pickers, and
unavailable accessibility providers retain native UI. Common translated Open/Save
button labels are included, but arbitrary application labels are not inferred.

The native dialog remains running. Immediately after successful recognition,
the worker hides the native window without disabling it, before starting the
FilePilot helper. The worker keeps the claimed dialog hidden while the picker
is active, including if the application shows it again. FilePilot is owned by
the application window rather than the hidden native dialog, so native-dialog
visibility changes do not control FilePilot's visibility. FilePilot's selected
absolute path is written to the native filename editor, an available type
selection is transferred, and the native Open/Save action is invoked. The native
dialog owns result buffers, actual option flags, application callbacks, overwrite
confirmation, and final acceptance. FilePilot itself does not write the selected
file. Cancel invokes the native Cancel action. A surviving dialog is revealed
after submission so application rejection and native confirmation remain usable.

Only safely enumerable file-type lists are reproduced completely. Otherwise the
existing selected type is retained. Patterns are inferred from displayed labels;
unknown patterns use `*`. If the current filesystem directory cannot be recovered
from the address controls, the picker starts in Documents. This initial external
mode selects one file per request; native multiselection flags are not recoverable
in every layout. Applications allowing multiple files can still receive one file.
The standard legacy read-only checkbox is mirrored when accessible. Additional
visible checkboxes/radio options cause native fallback. Other custom options are
not mirrored into FilePilot and remain available in native UI after fallback.
An initial native-window flash is possible because discovery
occurs after window creation.

## Recovery and deployment

Each claimed window is processed once until it closes; a worker's window property
and PID/thread/process-creation identity prevent handling a reused HWND as the
original request. Reopen a native fallback dialog to request interception again.
The watcher retains bounded status records and up to eight simultaneous workers.
Selected paths and titles are not written to diagnostic logs.
Fallback status identifies the failed recognition or delivery stage and includes
an error code when available, rather than reporting only a generic unsupported
dialog. Timeouts retain the stage at which the worker stopped responding.

UI Automation connection/transaction timeouts and bounded Win32 messages limit
provider waits. A worker heartbeat has a 10 second watchdog. Pause, Exit, process
loss, timeout, and worker failure restore a hidden native dialog. The watchdog
can terminate only its own isolated worker and verified private picker helper,
never the requesting application. Helper cleanup uses an open process handle,
its creation time, and the immutable picker executable path.
Cancellation events from workers are duplicated only from those owned workers.
If the watcher exits unexpectedly, the worker detects its controller's process
exit while pumping the picker and restores the native window.

Normal mode covers the current user/session and applications at no higher
integrity than the watcher. AppContainer, low-integrity, critical system and
explicitly excluded processes are left alone, except recognizable file-dialog
windows belonging to the exact System32 PickerHost.exe or RuntimeBroker.exe
binaries. Those Windows hosts may serve WinRT requests from another application;
the external layer preserves their native completion/result handling. Windows
access restrictions still apply. No automatic elevation or protection changes
are performed. Application-drawn or unrecognized brokered layouts retain native
UI. No FilePilot DLL is injected into a broker by this exception.

Keep the root executable, `com-runtime.txt` (with COM installed) or
`agent-runtime.txt` (standalone), and selected immutable runtime
directory together. The runtime contains its own worker executable and existing
picker payload. Stop the watcher before upgrading its root executable. Build and
package production targets using `build-com.ps1` for COM plus fallback, or
`build-agent.ps1` for standalone mode. No tests or validations are
written or run for this implementation; compilation does not establish runtime
coverage.

To return to the earlier injection mode:

```powershell
& .\FPilotDialogExternal.exe --uninstall
& .\install-com.ps1 -Action Uninstall  # If COM mode is installed.
& .\FPilotDialogAgent.exe --install
```
