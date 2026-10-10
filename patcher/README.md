# File Pilot Open File Location and tab-transport patch

This project creates one combined File Pilot 0.8.2 build with three related
features:

- it participates in the Windows shell-window protocol used by **Show in folder**;
- it preserves the pointer grip when a tab is torn into a new window, including
  signed cross-monitor placement and stable drag-helper positioning; and
- it lets a tab dropped on another patched File Pilot window join that window's
  tab group instead of spawning a third process. Ctrl preserves the source tab,
  matching File Pilot's native copy modifier.

The shell portion makes File Pilot participate in the Windows shell-window protocol used by
`SHOpenFolderAndSelectItems`. That is the API used by Chrome's **Show in folder** command and by
many **Open file location** actions.

## Root cause

File Pilot registers these replacement-shell commands:

- `HKCU\Software\Classes\Drive\shell\open\command`
- `HKCU\Software\Classes\Directory\shell\open\command`
- `HKCU\Software\Classes\CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell\opennewwindow\command`

All launch `"FPilot.exe" "%1"`. `SHOpenFolderAndSelectItems` gives a replacement file manager only
the parent folder in `%1`. Windows sends the child item afterward by locating the folder through
`IShellWindows`, asking the registered browser for its document/service provider, and invoking
`IShellView::SelectItem`. File Pilot 0.8.2 does not register those COM objects, so it opens the
correct folder but loses the selection request.

File Pilot's existing select-by-name startup command is not reusable for this callback. It runs
before asynchronous directory enumeration completes and is tied to a transient native command
queue.

Explorer++ solves the shell-protocol half by calling `IShellWindows::RegisterPending`, `Register`,
and `OnNavigate`, then exposing minimal `IWebBrowserApp`, `IDispatch`/`IServiceProvider`, and
`IShellView` implementations. This patch embeds the same facade and connects it to File Pilot's
live selection model.

Chromium has an additional compatibility requirement. Its Windows `Show in folder` implementation
parses the folder and file through the desktop `IShellFolder` before calling
`SHOpenFolderAndSelectItems`. For the same filesystem path, that can produce a canonical shell PIDL
which is not byte-identical to the compact PIDL returned by `SHParseDisplayName`. `IShellWindows`
uses the registered PIDL identity to find or complete a pending browser window. Registering only the
compact PIDL therefore opens File Pilot for Chromium but never delivers `IShellView::SelectItem`.

The payload registers the same File Pilot facade under both PIDL representations. Windows-style
requests can find the compact alias, while Chrome, Brave, and other Chromium browsers can find the
desktop/canonical alias.

## Cross-window tab merging

File Pilot's native command `0x58` can rearrange tabs only inside one process. Its destination
group and sibling are raw pointers, while every File Pilot top-level window is a separate process.
The input sampler also rejects a foreign HWND as an outside drop, so the stock handler always takes
the tear-off/CreateProcess branch.

The combined patch keeps the native serialization path and intercepts it immediately before the
new-window job is queued. If the release point is over another patched `File Pilot` main window,
the source sends the already-built `{ "Panel": ... }` JSON through a bounded, synchronous
`WM_COPYDATA` request. The target subclasses its main window on the UI thread, validates the sender
window, process ID, protocol version, and payload bounds.

While the button is still held, the patched native drag-image updater pulses a separate preview
session to the foreign window under the cursor. The receiver feeds the session's client point and
Ctrl state into File Pilot's input snapshot, supplies a non-dereferenced local drag sentinel, and
temporarily mirrors File Pilot's expected surface-generation value. Calls from all three native
panel surface paths are routed through that wrapper, so File Pilot itself draws its `TabDropMarker`
placeholder. No marker geometry or replacement UI is implemented by the patch.

The native render pass also records the target-local group and insertion sibling. At release the
receiver correlates that destination with the authenticated preview session and then:

1. resolves the target-local group and insertion sibling from the release point, panel bounds, and
   the same per-tab ImGui rectangles used by File Pilot;
2. parses the JSON with File Pilot's native open-window panel deserializer;
3. creates a blank tab in that group, applies the deserialized state, and focuses it; and
4. acknowledges success before the source continues.

For a move, File Pilot's existing queued close command executes only after that successful
synchronous acknowledgement. Ctrl-copy never queues the close. An unpatched, hung, invalid, or
incompatible target returns no acknowledgement, and the source falls back to the ordinary tear-off
window without losing the tab. Preview state expires after 250 ms, is ended explicitly on a
completed transfer, and falls back to release-time destination resolution if it cannot be safely
correlated.

## Binary integration

- Regression-tested binary: File Pilot 0.8.2 x64, SHA-256
  `08826147A90E7C6A1C4E80968AAA927B14CFBCA7271C7D12DB3AF9F24C483646`
- On 0.8.2, discovery resolves the startup hook to `0x1401DEED2`, the frame hook to
  `0x14019F465`, eight live-wrapper callers, the item renderer at `0x140116080`, the native
  selected-item opener at `0x14005DC00`, the close-all-tabs handler at `0x1400871F0`, and the
  panel loader at `0x14014E850`.
- Those addresses are regression expectations, not patcher inputs.
- Injected section: `.fplt`, with no new runtime DLL dependency
- Tab sections: executable `.fpt` plus process-private writable `.fpd`; no new
  runtime DLL dependency
- Cross-window transport: the `.fpt` drag-image and serialization bridges call the `.fplt` C++
  payload; the target receives on its existing main HWND, renders through File Pilot's native tab
  surface, and allocates process-local panel state only when the drop is accepted

The tab transport does not borrow bytes from File Pilot's queued job. Its JSON
begins at `job +0x58`, so placement data is held in a dedicated parent pending
record, transferred through `FilePilot-TearOffPlacementV2`, and copied into a
distinct child record. This both preserves the tab path and prevents the source
window from applying the child's placement.

The startup hook calls the original initializer first, then registers the File Pilot window and
startup folder with `IShellWindows` using both the compact `SHParseDisplayName` PIDL and the
desktop `IShellFolder::ParseDisplayName` PIDL. Both aliases expose the same COM facade. When Windows
calls `IShellView::SelectItem`, the payload combines the parent and child PIDLs, resolves the leaf
name, converts it to UTF-8, stores it as a synchronized pending selection, and wakes File Pilot's UI
loop.

The live-selection hook records File Pilot's persistent selection-state objects as they are
initialized. The frame hook retries the native selection function against those live objects until
the asynchronously enumerated item exists. This mirrors Explorer++'s important behavior: selection
is applied to the live browser model rather than sent through a startup-only queue.

## Update-resilient locator

For the `.fplt` shell payload, `patch_filepilot.py` does not use a target
hash or fixed File Pilot virtual address as a patching decision. It performs a
dry analysis before writing:

1. Masked instruction signatures locate the startup call, per-frame call, and live-selection
   wrapper. Relative addresses, stack sizes, and structure displacements are wildcarded.
2. The original initializer and frame function are derived from the located `call rel32`
   instructions.
3. Capstone disassembly of the live wrapper derives the native selector, selection commit helper,
   and selection/display structure offsets.
4. Independent structural signatures locate the inspector synchronizer, item renderer, and native
   **Open in Right Split** wrapper. Their instructions derive the owner/child links, item metadata
   fields, both directory-command sites, selected-item opener, active-panel field, and panel loader.
5. Calls to command `0x55` are correlated with the queued-command helper and event kind to locate
   native **Close All Tabs**; the selected-item opener's loader calls are captured structurally.
6. The x64 exception directory supplies verified instruction boundaries. Every direct call to the
   discovered live wrapper is collected and redirected, so the caller count may change.
7. LIEF resolves all required IAT slots by imported function name.
8. The next section RVA is calculated from the target's PE layout. The payload is relocated there,
   and an exported binding table is filled with the discovered functions, offsets, and imports.

The patcher requires unique signatures, plausible offsets, executable call targets, a bounded
number of wrapper references, and all required imports. Any missing or ambiguous relationship stops
the patch before an output is modified. A major compiler rewrite may require adding another
signature profile, but simple code movement, image-layout changes, changed IAT locations, changed
member offsets, and added or removed wrapper callers no longer require manually finding addresses.

Use analysis mode first on an updated executable:

```powershell
python .\patch_filepilot.py --analyze --open-location-only --layout-json .\layout.json ..\binaries\input\FPilot-new.exe
```

The JSON report records every discovered region and binding for review.

## Build

Prerequisites are Visual Studio with the x64 C++ toolchain, Python, LIEF, and
Capstone:

```powershell
python -m pip install lief capstone
& .\build_patch.ps1
```

If Python is not on `PATH`, the wrapper discovers the Codex runtime interpreter used by this
workspace. A different interpreter can be selected explicitly with `-PythonExe`.

The default input is `..\binaries\input\FPilot-original.exe`, and the default
output is `..\binaries\release\FPilot-open-location-tab-merge.exe`; the
original executable is read-only input. The Python patcher now emits both
`.fplt` and the tab tear-off sections in one invocation.

The explicit full composition is:

```powershell
& .\build_patch.ps1 -All
```

It writes `..\binaries\release\FPilot-all-patches.exe`, combining
Open File Location, tab creation/tear-off, cross-window tab merge, and the
Unicode payload. `-All` maps to the Python patcher's `--all` profile.

To emit only the Open File Location integration, use:

```powershell
& .\build_patch.ps1 -OpenLocationOnly -OutputExe ..\binaries\release\FPilot-open-location-only.exe
```

`-All` also compiles a second no-CRT payload into `.fpu`. The Unicode payload is
intentionally restricted to the verified File Pilot 0.8.2 hash because its text,
font, input, caret, and native quad seams use exact calling conventions and
structure layouts. The build writes `<output>.unicode.json`, which records the
fixed `native-row-resource` renderer and telemetry RVA.

The renderer shapes extended text with DirectWrite and caches each row as an
immutable native R8 texture descriptor. At File Pilot's verified pre-emission
quad seam, the Unicode render hook calls the native emitter directly with that
resource and its tight destination rectangle.
The resulting command is an ordinary native type-0 textured quad using File
Pilot's current color, clip, order, and menu/window transform.

There is no native-text bridge, invisible carrier, marker packet, frame hook, D3D draw-batch hook,
custom shader, or renderer selector. Cached row descriptors produce stable
commands, so File Pilot can use its existing unchanged-frame fast path.

Design details, coverage, and limitations are in
`..\docs\filepilot-unicode-patch.md`.

The shell locator remains structural. The tab hooks currently fail
closed unless the input is the verified File Pilot 0.8.2 SHA-256 because those
instruction seams depend on exact register and stack layouts.

Run the locator regression suite with:

```powershell
python -m unittest -v .\test_locator.py
```

The suite also applies the Python tab emitter to the known `.fplt` base and
requires the exact SHA-256 of the previously runtime-tested legacy tear-off
build. A separate regression enables the cross-window target and verifies that
the serialization stub contains a direct call to it. Together these guard the
hand-emitted x64 instructions, section layout, hook targets, private-state
offsets, and opt-in bridge behavior.

## Verification performed

The generated executable was tested through the real `SHOpenFolderAndSelectItems` API in several
scenarios:

- A cold call launched File Pilot through the registered shell command and selected a non-default
  second row.
- A later call reused the same File Pilot window and changed the selection to a different file.
- A populated Downloads folder selected an MSI among 123 files and scrolled it into view.
- Chromium's desktop-parsed PIDL path selected an MSI on cold launch, reused that window for a
  second EXE selection, and returned `S_OK` both times.
- Brave's real Downloads-page **Show in folder** buttons selected the MSI and then the EXE in one
  File Pilot process.
- A Windows-style request successfully reused the Chromium-launched window through its compact PIDL
  alias.

All calls returned `S_OK`; the application remained responsive and closed normally.

`test_select.cpp` is the Windows-style API reproducer. `test_chromium_select.cpp` mirrors Chromium's
desktop-folder PIDL construction. `capture_window.ps1` was used only for visual verification; none
of these test utilities are required by the patched executable. `test_locator.py` verifies masked
signature relocation, fail-closed signature behavior, complete binding generation, and exact
rediscovery of the required shell and tab-support regions in the 0.8.2 regression layout.

The earlier relative-placement build was additionally exercised from the original
executable and produced SHA-256
`D5812372BCD1C829B739852F75C3AEA100D0F9F1F8D2EC6F4EC8C47AE210F181`.
In a multi-tab drag the source remained at `(-2432,570)` and cursor
`(1280,576)` placed the child at `(980,557)`, preserving anchor `(300,19)` and
the `selection_test` path. Both standard and Chromium-style
`SHOpenFolderAndSelectItems` probes returned success; the latter visibly
selected `SECOND_TARGET.log`.

The current separated build was generated from the original 0.8.2 executable as
`binaries/release/FPilot-open-location-tab-merge.exe`, SHA-256
`AD18C1C56891F467658612BE20402C8FB46D8DD7CA37AE3A5435A818726A5C56`.
A controlled two-process drag displayed File Pilot's native blue insertion placeholder in the
receiver. Runtime telemetry recorded accepted preview sessions, live cursor updates, native tab
surface calls, and captured insertion destinations.

## Important notes

- Patching invalidates Voidstar's Authenticode signature. The output is intentionally unsigned;
  Windows may show an unknown-publisher warning.
- Compatibility is structural rather than hash-based. Passing discovery proves that the required
  regions still have the relationships used by the payload; it is not a promise that an arbitrary
  major rewrite is compatible.
- No registry changes are made by the patcher itself. To replace an installed copy, close every
  File Pilot process, keep a backup, substitute the patched executable, and re-enable File Pilot's
  default-file-manager integration if its registered path changed.

# Undo/redo

For File Pilot 0.8.5, `build_patch.ps1 -All` includes archives, startup optimization,
and shared undo/redo history; `-Undo` enables history separately. No adjacent undo
DLL is required. Ctrl+Z undoes; Ctrl+Y or Ctrl+Shift+Z redoes. File-move drops are
recorded in shared session history across FilePilot processes. Replay submits one
Shell operation per batch. Close all FilePilot processes when upgrading.
Runtime validation of the latest composition has not been performed.

## Open and Save As replacement

`build_patch.ps1 -All` now also builds the separate dialog package. Use
`-Dialogs` alongside an executable patch profile, or `-DialogsOnly` to build
just the dialogs. Add `-WinRT` to package the optional Windows Runtime provider;
`-DialogOutputDirectory` selects the output folder. The normal patched executable
and dedicated picker remain separate, and the picker requires the original
FilePilot 0.8.5 input.

```powershell
& .\build_patch.ps1 -DialogsOnly -WinRT
& ..\binaries\release\dialogs\install-com.ps1 -Action Install
# Optional WinRT registration, from elevated 64-bit PowerShell:
& ..\binaries\release\dialogs\install-winrt.ps1 -Action Install
```

Builds compile production targets only and leave registration unchanged.
COM replacement uses ordinary activation for the current user; an external
watcher covers recognizable native dialogs that bypass it and can still flash.
The WinRT provider has a separate reversible machine registration. Neither mode
remotely injects a DLL or replaces Windows system DLL files. The retained API
injection agent is an explicit alternative mode. Installation, recovery, native
UI behavior and coverage limits are documented in [dialogs/README.md](dialogs/README.md).
