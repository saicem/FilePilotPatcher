# FilePilot Open / Save As replacement

The default package combines a modern-dialog COM replacement with an external
fallback watcher. Supported filesystem `IFileOpenDialog` and `IFileSaveDialog`
requests open FilePilot before Windows displays a dialog. Windows loads the COM
bridge normally into the caller; the package does not remotely inject it or
require applications to use a launcher. Recognizable legacy and brokered native
file dialogs use the external watcher after creation and can briefly flash.

The optional WinRT provider additionally replaces ordinary
`Windows.Storage.Pickers.FileOpenPicker` and `FileSavePicker` operations for
supported full-trust desktop callers. Sandboxed calls retain Windows because
path selection alone cannot provide their brokered file-access grants.
Application-drawn dialogs and unsupported contracts keep their original UI.
This package does not guarantee universal suppression.

## Build and installation

From the repository root:

```powershell
# Dialog package only: x64/x86 COM bridges, external fallback, native picker.
& .\patcher\build_patch.ps1 -DialogsOnly

# Compose the normal executable patches and build the dialog package alongside.
& .\patcher\build_patch.ps1 -All

# Also package the optional WinRT provider.
& .\patcher\build_patch.ps1 -DialogsOnly -WinRT
```

The direct equivalent is `patcher/dialogs/build.ps1`, with `-IncludeWinRT` for
the additional provider. `-InputExe`, `-OutputDirectory` and `-PythonExe` can be
specified there; the main wrapper uses `-DialogOutputDirectory` for the package.
Visual Studio C++ tools and Python with LIEF are required. The dedicated picker
must be built from the verified original FilePilot 0.8.5 x64 input, never from
the combined patched executable. FilePilot retains its vendor license; MinHook's
license accompanies the picker runtime.

Builds compile production targets only. They do not run tests, launch application
dialogs, register providers, or change the installed runtime. Generated packages
are local build output, excluded from Git. Each COM/WinRT build publishes a new
immutable payload and a manifest; existing payloads remain available to callers
that still have their DLLs loaded.

Install or upgrade from the generated package:

```powershell
& .\binaries\release\dialogs\install-com.ps1 -Action Install
```

This installs both COM architecture views for the current user and starts the
external fallback at sign-in. Fully close and reopen applications to release
cached factories and older loaded bridges. See [COM-README.md](COM-README.md)
for Enable, Disable, Uninstall, exclusions and ownership-aware recovery.

The optional WinRT installation is a separate command in elevated 64-bit
PowerShell after a build with `-WinRT`:

```powershell
& .\binaries\release\dialogs\install-winrt.ps1 -Action Install
```

See [WINRT-README.md](WINRT-README.md) for machine registration, recovery and
native delegation. It changes activation paths, not Windows DLL files. Both
providers execute code in the caller through ordinary activation/loading.

## Picker behavior and coverage

FilePilot provides navigation, the file list and the footer through its native
immediate-mode UI tree, palette, fonts, buttons and text-editor renderer. There
is one FilePilot window, with space reserved for the footer and native popups
above it. Context-menu commands and file-management operations retain their
original FilePilot handlers, including rename, folder creation, copy/move/delete
and properties. Explicit file-management actions are separate from accepting a
picker result.

Filename selection initially covers the basename. Double-click selects the
basename or extension according to the clicked position; triple-click selects
the whole filename. Tab/Shift+Tab navigate the browser and footer, F4 opens file
types, Enter accepts, Escape closes the type menu or cancels, and Ctrl+L returns
to the address bar. Unicode, IME, clipboard shortcuts and undo/redo use the native
editor. The filename field accepts up to 65,532 UTF-8 bytes.

Picker settings are isolated beneath
`%LOCALAPPDATA%\Voidstar\FilePilotPicker\Sessions`. COM Save returns a path for
the application to write. WinRT Save creates a missing destination to return a
genuine StorageFile without truncating existing contents. Packets support up to
128 filters or selected paths and 512 KiB of serialized data.

Unsupported options, advanced custom controls, editable Save metadata, virtual
Shell items and inaccessible providers retain Windows behavior. Read-only/innate
property collections without appended default editors can use FilePilot; this
includes Paint's encryption-owner property request. The external watcher cannot
hide a native window before it exists. See the
[coverage and Paint analysis](../../docs/filepilot-dialog-coverage.md).

## Alternative delivery modes

Source for the earlier API-injection agent and per-application launcher is kept
for explicit alternative use. It is not enabled by the default COM installation.
See [AGENT-README.md](AGENT-README.md) and
[EXTERNAL-README.md](EXTERNAL-README.md). The injection agent requires uninstalling
COM replacement first; disabling COM alone retains its registration. The agent
payload can be built separately with `build-agent.ps1` after building the picker.

No tests or functional validations are part of this update. Production compilation
does not establish runtime coverage for every application or dialog contract.
