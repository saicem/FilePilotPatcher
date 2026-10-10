# FilePilot Open and Save As dialog implementation scheme

Investigated 2026-10-09. Scope: the application's file-selection **Open** and
**Save As** dialogs, as clarified by the user.

For automatic coverage of applications opened normally, including existing
processes, see [FilePilot dialogs without an application launcher](filepilot-universal-dialog-scheme.md).
That scheme supersedes the launcher-only deployment scope below; the picker and
adapter architecture remains applicable.

## Recommendation

Implement two cooperating components:

1. A dedicated `FPilotPicker.exe`, built from the supported FilePilot executable
   with a session-specific picker payload. It supplies FilePilot's actual browser,
   navigation, selection, sorting, and preview UI plus a filename/type/action footer.
2. An adapter DLL loaded into participating applications. It translates modern
   `IFileOpenDialog` / `IFileSaveDialog` and legacy `GetOpenFileName` /
   `GetSaveFileName` calls into picker sessions, then returns the exact result format
   the application expects.

This is feasible for ordinary desktop filesystem dialogs. Patching FilePilot alone
does not intercept dialogs created inside other applications. Its default-file-manager
registry integration and `SHOpenFolderAndSelectItems` facade address a different
protocol. Broad deployment therefore requires loading the adapter in each covered
application, as well as extending FilePilot.

Start with an explicit application allowlist and launcher that loads the adapter
before the application begins executing. Expand deployment after compatibility
tests. Avoid replacing Windows DLLs or globally overriding the two dialog COM
classes as the first implementation: either approach expands the failure domain,
and a COM override still misses legacy dialogs and other picker technologies.

## Evidence and available native seams

The workspace includes Ghidra 12.1.2 and two analyzed programs:

- `ghidra/project/FilePilot.gpr`: original 0.8.2 analysis.
- `ghidra/project/FPilot 0.85.gpr`: original 0.8.5 analysis.

Use the 0.8.5 input with SHA-256
`ab5e7bbfa50872225f24856697ef7341a58e51a5e521435ba0dfeeb8ab3b2676`.
The addresses below are image VAs at base `0x140000000`. They describe this build,
not arbitrary releases. Runtime code must use resolved RVAs and verified layouts.

| Purpose | Verified address/layout | Evidence |
| --- | --- | --- |
| Window/panel initialization call | `0x1401E15FB` calls `0x140186860` | Fresh patcher analysis in `native-layout.json` |
| Main frame call | `0x1401A17F7` calls `0x140167ED0` | Fresh patcher analysis |
| Active panel | `app + 0xAF8` | Fresh locator and sidebar decompilation |
| Selected-item Open dispatcher | `0x14005E090`, ABI `(app, panel, int* options)` | Existing archive decompilation and adapter |
| Selected-item path descriptor | `0x140067400`, ABI `(model, item)` | Fresh Ghidra survey and existing adapter |
| Sidebar/recents path activation | `0x14001C870`, ABI `(app, UTF8 pointer/length, isDirectory)` | Fresh Ghidra survey |
| External file-open worker dispatch | `0x14010EEB0` queues/calls `0x140204EA0` | Fresh Ghidra export |
| Native folder chooser | `0x140113430` | Existing Ghidra decompilation |
| Main HWND | platform pointer at `0x140249240`, HWND at `platform + 0x8C0` | Existing startup payload and folder chooser |
| Panel browser model | `panel + 0x90` | Archive binding profile |
| Cursor / selected rows | cursor at `+0x128`; selection mode `+0x120`; count `+0x178`; array `+0x1B0`; record stride `0x10` | Archive profile and native Open decompilation |
| Path descriptor fields | UTF-8 pointer `+0x10`, length `+0x18` | Fresh descriptor decompilation and archive adapter |

Native item flags at `item + 0x20` use bit `0x2` for directory handling. These are
FilePilot flags, not Win32 `FILE_ATTRIBUTE_DIRECTORY`. Existing archive support
can mark a physical archive as a navigable directory; picker acceptance must
therefore also classify the physical path.

FilePilot's own native folder chooser creates `CLSID_FileOpenDialog`, queries
`IFileDialog`, and sets `FOS_PICKFOLDERS` (`0x20`). It is a consumer of the Windows
dialog. It is not an existing FilePilot Open/Save picker service. PE inspection
found the FileOpenDialog GUID and no literal FileSaveDialog GUID or legacy
GetOpenFileName/GetSaveFileName imports. Those negative findings do not exclude
dynamic calls, but no reusable Save picker mode was established.

The native filter/header UI is reachable through `Panel_FilterHint` references in
`0x1401356A0`. The subsequent native UI investigation traced the custom
immediate-mode tree through context global `0x140249278`. The picker now uses
root begin/end at `0x1401CE3E0` / `0x1401CEBD0`, widget creation at
`0x1401CA2F0` / `0x1401CA3B0`, parent/layout binding at `0x1401CE850`, native
buttons at `0x1401D90E0`, text editor rendering at `0x1401DD2F0`, and mouse
selection at `0x1401D3600`. These are FilePilot-specific ABIs, not Dear ImGui
exports. Details and decompilation evidence are in `analysis/dialogs-native-ui`.

## FilePilot picker mode

Use a dedicated helper copy and add an internal entry option such as
`--fp-picker-v1 <session-token>`. Consume this option in the payload before native
startup. Feed native startup an effective command line containing only the initial
folder; FilePilot's current parser expects a single path argument. The existing
startup payload already demonstrates replacing the effective command line.

Keep the picker session alive until the host accepts or cancels it. Initially use
one helper process per dialog. Do not reuse an ordinary FilePilot window: its
tabs, selection, lifetime, and configuration are independent of the modal request.

For the browsing surface, retain native enumeration and navigation. Add a footer
containing filename entry, file-type selection, Open/Save, Cancel, and validation
feedback. Reserve layout space before native panel layout so rows never sit
under the footer. Insert the controls within FilePilot's existing UI frame using
its widget ABI. The implemented native footer uses FilePilot's font scale,
palette, layout, buttons, text rendering, and mouse selection. A subclass of the
original FilePilot window forwards session messages and handles footer keyboard
input; no additional host window or Win32 child controls are created. The browser
tree is wrapped in a smaller anonymous container before layout, preserving its
relative widget IDs. The footer and file-type popup are siblings under the full
native root. The native UI update was compiled only; no new tests or validations
were written or run, at the user's request.

Intercept selected-item activation at `0x14005E090` in picker sessions:

- A folder activation continues through native navigation.
- An Open file activation snapshots its full path and proposes acceptance.
- In Save mode, selecting a file updates the filename field; the Save action
  proposes the destination. Double-click behavior must be explicitly defined and
  consistent with the chosen dialog contract.
- Read the whole selection when multi-select is enabled. Enforce single selection
  otherwise. Snapshot strings on the FilePilot UI thread; never send native item
  pointers or retain them across enumeration changes.

Also handle the sidebar/recents activation path at `0x14001C870`; otherwise a
recent file can launch an external app. Audit context-menu verbs, previews,
keyboard commands, and the external-open worker as alternate activation routes.
Route Enter, Escape, the action buttons, and window Close through one session
state machine. Make destructive file-manager commands unavailable in picker mode.

Apply file-type patterns to the visible file rows while always leaving navigable
folders available. Combine these with the user's search filter; do not substitute
a wildcard string into FilePilot's text filter and assume identical semantics.
Validate acceptance independently of visibility, including typed filenames.

Keep session settings outside the ordinary window/layout persistence. The helper
must not register as the default file manager, publish ordinary shell-window
aliases, adopt the normal warm-window mailbox, or save picker tabs into the normal
layout. Shared initialization/frame hooks must be composed through a single
dispatcher with the existing startup/archive payloads rather than installing
competing detours. Begin with an original-based picker build and add optional
patches only after its baseline works.

## Adapter design

### Modern COM dialogs

Intercept `CoCreateInstance` for these two CLSIDs in covered processes only:

- Open: `{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}`.
- Save: `{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}`.

Return a COM proxy implementing the applicable Open/Save interface and the common
dialog interfaces. Capture configuration calls: options, file types/index, default
extension, filename, folder/default folder, titles, button labels, places, and
client GUID. Preserve COM identity and reference counting. Retain a configured
native dialog for fallback. `Show()` launches the helper and runs a modal wait;
the proxy supplies result getters and invokes event sinks on the caller's apartment.

`Show()` alone is insufficient. Implement `GetResult`, Open `GetResults` and
`GetSelectedItems`, `GetFolder`, `GetFileName`, `GetFileTypeIndex`, current
selection, and `Close`, with correct validity states. Include `Advise`/`Unadvise`.
During acceptance, stage results before `OnFileOk`; a sink's rejection leaves the
helper open. Propagate supported folder/selection/type events and honor folder
navigation vetoes. Sink callbacks can change state or call `Close`, so IPC waiting
must allow reentrancy. Microsoft documents this lifecycle and the Shell-item
result contract in [Common Item Dialog](https://learn.microsoft.com/en-us/windows/win32/shell/common-file-dialog)
and [GetResult](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-getresult).

Return real Windows `IShellItem` objects, and a real Shell item array for multi-select.
For a nonexistent Save target, use `SHCreateItemFromParsingName` with a bind context
containing `IFileSystemBindData` under `STR_FILE_SYS_BIND_DATA`. A compiled probe
in this investigation returned `S_OK`, reproduced the requested filesystem path,
and verified the target remained absent. This mechanism is documented by
[SHCreateItemFromParsingName](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-shcreateitemfromparsingname)
and [IFileSystemBindData](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifilesystembinddata).

Treat the interface surface as a compatibility requirement. Apps may query
`IFileDialog2`, `IOleWindow`, customization interfaces, or use Save metadata methods.
Implement a coherent interface family; do not return an unwrapped native interface
that bypasses the proxy or merely report success for unsupported methods. Determine
the supported profile before showing the helper and use the retained native dialog
for incompatible requests. Customization controls, property editing, item filters,
and virtual Shell items require further implementation before broad coverage.

Later cover `CoGetClassObject` / factory `CreateInstance` and other activation
paths observed in target apps. A CoCreateInstance-only prototype is not universal.

### Legacy common dialogs

Hook `GetOpenFileNameW/A`, `GetSaveFileNameW/A`, and `CommDlgExtendedError`.
Normalize input into the same session model. On success, fill `OPENFILENAME`
outputs, including filter index, offsets, title, and applicable output flags.
For Explorer-style multi-select return `directory\0leaf1\0leaf2\0\0`; a single
result is a full path. Use the caller's character encoding for A APIs and check
buffer sizes in the correct units. Preserve the documented small-buffer error and
thread-local extended-error behavior. Cancellation returns FALSE with no extended
error. See Microsoft's [OPENFILENAMEW contract](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamew).

Initially fall back for custom hook procedures/templates and old-style dialogs.
Ensure the TLS error override clears when an unhandled native call runs. Use a
thread-local recursion guard so legacy internal calls into modern dialog APIs do
not create two picker sessions. Module import patching alone misses dynamic and
late-loaded callers; use scoped function detours or a complete loader-aware scheme.

## IPC, modality, and result handling

Use a versioned local named-pipe protocol. Launch the helper from a fixed path;
send request data over the pipe rather than putting filenames into executable
command text. Restrict the endpoint to the intended logon/session, authenticate
the peer process and session token, and use bounded messages with explicit lengths.
Keep the helper at the caller's applicable integrity level and privilege context.

Request fields: session ID, mode, owner HWND/PID, title, initial/default folder,
initial filename, default extension, ordered filters and selected index, flags,
places, and capabilities. Events carry folder/type/selection changes. Result
fields carry status, paths, final folder, selected filter, and any supported
output flags. Carry strings and stable control IDs, never COM or native pointers.

Acceptance is a handshake: helper proposes paths; adapter constructs the results,
validates them and runs caller callbacks; adapter replies accept/reject; helper
closes only on accept. Preserve exactly one terminal outcome. Escape, X, owner
death, pipe loss, and `IFileDialog::Close` must terminate the session predictably.

The helper is an owned top-level window, not a reparented child of the application.
Disable the owner only if appropriate and restore its previous state on all exits.
The caller's UI thread must pump messages/COM while waiting, preserving WM_QUIT and
supporting callback reentrancy. Detect helper death and startup failure. Startup
failure can use native fallback; user cancellation must remain cancellation.
Avoid a short overall timeout while a user is browsing.

Save As returns a destination; the application writes the file. Implement default
extension/type behavior, existing-parent checks, overwrite confirmation and
callback responses, readonly/link/hidden-item policy, and the applicable validation
options. Do not truncate or create the destination merely to form the result.
If a documented writeability test is needed, implement it separately with the
correct flags. Preserve shortcut-target behavior and distinguish physical
filesystem paths from archive virtual paths. Initially use native fallback for
Shell namespace/stream-only requests. The option meanings are documented in
[FILEOPENDIALOGOPTIONS](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions).

## Implementation order and acceptance criteria

1. Build a picker-session harness against original 0.8.5. Verify initial folder,
   native navigation, selected-path extraction, cancellation, and lifecycle.
2. Trace the exact native UI/layout and row-filter seams. Add the footer and
   Open/Save state machine. Verify file activation does not launch another app.
3. Add a standalone modern COM test host and adapter: single Open, multi Open,
   folder mode, existing and nonexistent Save targets, cancellation, and callback
   rejection. Confirm results with the same APIs the real applications use.
4. Add legacy W/A handling with buffer/encoding/error cases. Add app-specific
   profiles and native fallback, then expand the application allowlist.
5. Test the browser process of Chromium/Electron apps, WinForms/WPF, and selected
   native desktop apps; verify Office and other custom surfaces separately.

Required cases include Unicode and long paths; UNC; filters and default extensions;
shortcut flags; enumeration races; mixed selections; overwrite yes/no; multiple
dialogs; callback-induced state changes; owner/helper death; and unsupported
customization fallback. Validate x64 first; an x86 app requires an x86 adapter even
when the helper remains x64. WinRT/WinUI pickers, protected processes, and applications
with custom file dialogs require separate interception paths. Do not claim every
Windows prompt is replaced after the standard API prototype passes.

## Windows binaries and what remains unknown

No Windows binary import into Ghidra is required to implement the recommended
public-API adapter. Read-only registry inspection on this machine found both
dialog classes hosted by `C:\Windows\System32\comdlg32.dll`, with Apartment threading.
The OS reports 25H2, build `26200.9457`; the actual comdlg32 file version is
`10.0.26100.8117`. Module versions need to be captured individually rather than
inferred from the OS build.

If pursuing an internal Windows dialog hook to preserve more native behavior,
the first useful Ghidra import is this machine's **System32/comdlg32.dll**, with
matching symbols. Add **SysWOW64/comdlg32.dll** for x86 callers. Only add shell32,
explorerframe, or combase when traces show they are needed. That internal approach
needs build-specific call-path analysis; it has not been established here.

The implementation below now covers the public-API prototype, x86/x64 loading,
picker-only settings and basic command suppression. Remaining work includes the
native footer/widget ABI, broader COM compatibility, elevated application coverage,
and visual and real-application verification.

## Implemented opt-in prototype (2026-10-09)

Source and build instructions are in `patcher/dialogs/README.md`. The local package
is `binaries/release/dialogs/`. It includes x64/x86 launchers and adapters, a normal
sidecar runtime DLL, and a hash-gated dedicated copy of FilePilot 0.8.5. The original
input and installed copies are not replaced.

The adapter hooks COM creation and factory activation plus legacy W/A Open/Save
APIs. Ordinary filesystem dialogs are proxied; unsupported customizations and
options use the native dialog. A suspended opt-in launch initializes the Windows
loader in a short-lived secondary thread, loads the adapter, installs its hooks,
and then resumes the application entry thread. Descendants and existing instances
are not automatically injected.

Sessions use anonymous shared mappings and events inherited through an explicit
handle list instead of the proposed named pipe. The request, event and reply
packets are bounded UTF-16 data; no public named IPC endpoint is registered.
The calling thread pumps messages, disables/restores the owner window, and owns
helper cancellation and cleanup. COM results are real Shell items and item arrays;
nonexistent Save targets use filesystem bind data without creating the destination.

The browser is embedded only after native initialization: creating it as a child
too early triggers FilePilot's frame loop before its app model exists. Native
frame, file-open and sidebar seams provide selection/navigation updates. A Win32
footer supplies filename, file type and action controls; an NtQueryDirectoryFile
wrapper filters synchronous listings. Asynchronous/unrecognized enumeration
formats retain native behavior. Direct file mutation, process-launch and registry
APIs are suppressed in the helper; Shell extensions are not sandboxed.

Launcher-injected x64 and x86 conformance runs each passed 93 assertions. Reports
are `analysis/dialogs-runtime/fixtures-complete-x64/conformance.txt` and
`fixtures-complete-x86/conformance.txt`. They cover API results and real helper
sessions, not visual layout or broad third-party app compatibility. Computer-use
could not connect, so interactive verification is still outstanding. Public API
contracts were checked against Microsoft's [IFileDialog::Close](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-close),
[OnFolderChanging](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialogevents-onfolderchanging),
and [OPENFILENAMEW](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamew)
documentation. Additional interfaces and all Windows picker types are not claimed.

## Reproducible evidence

New artifacts are in `analysis/file-dialog-0.8.5/`:

- `survey.txt`, `picker-paths.txt`: read-only Ghidra exports from the 0.8.5 project.
- `native-layout.json`: fresh `patch_filepilot.py --analyze --open-location-only`
  result against the original input; no patched executable was generated.
- `binary-evidence.json` / `inspect_binary.py`: PE imports and literal GUID hits.
- `windows-evidence.json` / `inspect_windows.ps1`: COM registration and module versions/hashes.
- `SaveShellItemProbe.cpp`, executable, and `save-shell-item-probe.txt`: the
  successful nonexistent-Save-target Shell-item probe. This checks result
  construction, not full dialog or app compatibility.

`ghidra/scripts/FilePilotDialogSurvey.java` is the new read-only export script.
The saved consoles contain the invocation parameters. Headless batch commands
returned exit code 1 after producing the requested exports without a reported
script exception, so that exit status is not treated as an integration-test pass.
Existing Ghidra databases, application executables, and Windows registration were
not modified by this investigation.
