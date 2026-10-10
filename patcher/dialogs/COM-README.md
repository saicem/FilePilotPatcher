# FilePilot modern-dialog COM replacement

The separate [WinRT provider](WINRT-README.md) can additionally replace ordinary
Windows.Storage.Pickers Open/Save calls for full-trust desktop applications.
It has its own machine installer and recovery journal; this COM installer does
not install or remove it. Disabling COM also gates the WinRT replacement when
both are installed.

This mode registers separate x64 and x86 FilePilot COM servers for Windows'
FileOpenDialog and FileSaveDialog classes, for the current user. Windows loads
the bridge normally when an application activates either class. It uses no
remote-thread injection, API detours or application launcher.
The bridge still executes custom code inside each caller, and security policies
can reject its loading. It is a COM registration override, not an official
Windows file-picker provider extension point.

Installation also starts a separate external watcher for recognizable native
Open/Save windows that bypass COM. This extends coverage to legacy APIs and
standard Windows picker-host dialogs without attaching FilePilot code to those
processes. COM handles supported calls before display; external fallback detects
the native window after creation, hides it, and submits the FilePilot selection
through its original controls. A brief native flash remains possible on this
fallback path. This is broad desktop coverage, not every application-drawn UI.

## Install and remove

From the packaged `binaries\release\dialogs` directory:

```powershell
& .\install-com.ps1 -Action Install
```

Installation exits this package's API agent, old external watcher and attachment
hosts, publishes the new payload, then starts the updated external fallback.
It removes the older matching agent sign-in command, preserving a raw copy for
uninstall. The fallback uses the separate `FilePilot Dialog Fallback` Run entry.
Controllers belonging to another package are not stopped.
Applications are not terminated. Fully close and reopen Brave and other
applications once: existing injected adapters and cached COM factories cannot
be replaced by changing registration. COM itself may cache class factories.

The COM registration remains active across sign-in; the fallback watcher starts
at sign-in as well. Its workers and picker use the selected `com-runtime.txt`
payload, so they receive the same filename/UI corrections as COM dialogs.
Keep its immutable `ComRuntime-1-*` folder in place while registered or while
applications still have the bridge loaded. No Windows DLL is patched or replaced.

```powershell
& .\install-com.ps1 -Action Disable   # Keep registration; delegate to Windows.
& .\install-com.ps1 -Action Enable
& .\install-com.ps1 -Action Uninstall
```

Uninstall stops the fallback and removes its owned startup command, then
restores the original per-user server subtree, or its absence, in
both architecture views. It restores removed startup values only if their names
are still absent. It does not automatically restart the previous controller.
The injection API agent still refuses activation while COM mode is installed.
The external watcher can coexist with COM and suspends/cancels its requests
when COM replacement is disabled. Its own tray Pause applies to fallback only.
Uninstall COM mode before explicitly returning to the injection API agent.

## Backup and recovery

`HKCU\Software\Voidstar\FilePilotDialogs\ComRegistration.v1` is the durable
installer journal. It stores complete raw copies of the original per-user CLSID
trees, including value types, bytes and subkeys, and records each architecture's
installed server path. The installer modifies only `InprocServer32`, adding an
ownership value plus the server path and `Apartment` threading model.

Restore removes only an owned server subtree. Other CLSID subkeys created later
are retained. If another program changes ownership or the registered server
path, uninstall retains the journal and reports a conflict instead of replacing
that program's registration. Do not delete a retained recovery journal. After
an interrupted installation, use `-Action Uninstall` before installing again.
Failed install attempts perform the same ownership-aware restoration when
possible. Original machine registrations are never modified.

Policy values `ComModeInstalled` and `ComEnabled` live in the 64-bit view of
`HKCU\Software\Voidstar\FilePilotDialogs`. The bridge also reads the existing
UTF-8 application exclusion file at
`%LOCALAPPDATA%\Voidstar\FilePilotDialogs\exclusions.txt`. Excluded applications
still load the registered bridge; it delegates their dialogs to Windows.
Missing picker support, custom controls, unsupported options, virtual Shell
items and editable Save metadata preserve the adapter's native fallback.
Property collections containing only Windows-described read-only/innate
properties, with no appended file-type defaults, can use FilePilot. Paint's
System.Security.EncryptionOwners collection is one such request. Configuration
is still forwarded to the native object; GetProperties returns the selected
file's collected read-only values through a property store, and ApplyProperties
remains delegated to Windows. The picker does not modify encryption or metadata.

FilePilot's native context-menu dispatch and file-management operations are
enabled inside the picker, including renaming existing entries and creating
folders. Browser/sidebar file activation still selects a dialog result. The
footer relinquishes focus on browser right-clicks and passes keyboard input to
native popups while their higher input layer is active. Save still returns a
destination for the calling application to write; context-menu operations act
only when invoked by the user. This does not change COM registration coverage.

## Implementation and coverage

`FPilotDialogCom64.dll` and `FPilotDialogCom32.dll` export `DllGetClassObject`
and `DllCanUnloadNow`. Each factory creates the native Windows object by directly
calling the matching system `comdlg32.dll` factory, then wraps supported
interfaces with the shared `modern_dialog.cpp` implementation. Native fallback
does not activate the overridden CLSID again. Unknown initial interface requests
and aggregation are delegated directly to Windows. Module accounting covers
factories, wrapped dialogs, server locks and factory entry-point execution;
modal Show retains the wrapper across application callbacks. The Windows module
is retained for the process lifetime to protect native objects returned directly.

The FilePilot picker and its native UI runtime are resolved beside the bridge,
independently of the injection agent's manifest, heartbeat and policy mapping.
No MinHook, remote attachment or child-process delivery code is linked into the
COM server. Supported modern calls open FilePilot before native Show is invoked.
Legacy GetOpenFileName/GetSaveFileName, WinRT activation, private factories and
registration-free activation bypass this COM path. The external watcher handles
their windows when they expose a recognizable standard native file dialog.
It also permits external automation of the exact System32 PickerHost.exe and
RuntimeBroker.exe binaries in the same user/session, subject to Windows access
checks. Custom application UI, unsupported layouts/options and inaccessible
providers retain native dialogs. A process that rejects the registered DLL may
fail activation before there is any native window for fallback to intercept.

## Build

```powershell
& .\build-com.ps1
```

This compiles production targets for both architectures and publishes a new
immutable payload followed by `com-runtime.txt`. It does not register COM,
build or run test targets, or exercise application dialogs. The existing picker
executable is packaged with the newly compiled runtime. Installation copies the
updated guarded controllers only after their previous versions have exited.

No tests or validations were written or run for this implementation, as requested.
