# Picker context-menu and rename correction

The COM replacement remains the active Open/Save delivery mechanism. This change
is confined to the separate FilePilot picker runtime and its footer input routing.

## Cause

The original selection-only runtime hooked FilePilot's imported CreateFileW,
DeleteFileW, MoveFileExW, CreateDirectoryW and CreateProcessW functions, denying
filesystem writes outside its session directory and denying process launches.
Its CoCreateInstance hook denied CLSID_FileOperation as well as dialog classes.
Those restrictions prevented native FilePilot actions from completing, even
though their menus and editors could be rendered. Existing Ghidra exports in
`analysis/undo-0.8.5/operations.txt` show native COM operation activation and
direct file-operation calls. These were static source/binary findings.

The ShellExecuteW hook discarded the requested verb and arguments, returned a
success-like value and queued the path as a file-picker acceptance. Properties,
explicit Shell actions and other commands therefore could not retain their
original behavior. Native file-selection interception already exists at the
browser's selection dispatcher (RVA 0x5e090) and sidebar dispatcher (0x1c870),
so intercepting ShellExecute globally was unnecessary.

The footer tracked mouse focus only on left-button presses. Save starts with
filename focus, and right-clicking a file did not hand keyboard ownership back
to the browser. The footer also interpreted Escape/Tab before native popup
handling. Ghidra's native UI lifecycle export identifies the input floor at
UI context offset 0xd8 and native input layers at 0x1790.

## Change

Remove the file-operation, process-launch, COM-activation and ShellExecute import
overrides and their dead forwarding/denial code. FilePilot now dispatches native
commands using its original handlers and normal Windows APIs. No replacement
rename implementation, custom context menu, or application injection is added.
The onOpen/onSidebar selection adaptation, filtering, native footer, private
session configuration, registry-write suppression and isolated instance mutex
remain. Save acceptance continues to return a path to the application rather
than creating/truncating the destination.

The footer updates mouse focus on right/middle clicks as well as left clicks.
Keyboard and committed IME messages pass to FilePilot while a higher native
input floor owns interaction, except when the picker's own type dropdown is
active. This lets native popup editors receive their own commit/cancel keys.

## Delivery

Compile the production runtime and publish a new immutable COM payload with
`build-com.ps1`, then upgrade through `install-com.ps1 -Action Install`. Existing
registration backups and installer ownership are retained. Old payloads remain
available to processes caching their previous COM factories. Fully close and
reopen applications to select the new payload; no applications are terminated.
Already open picker windows retain the old runtime until that picker closes.

Installed on 2026-10-10 through the COM installer upgrade. Both architecture
registrations now select `ComRuntime-1-20261010-055218-6406136`; the existing
ownership identifier and original registration backups were preserved. Only
the x64 picker runtime required recompilation; both callers use that helper.

No tests, validations, application-dialog trials, renames or other user file
operations were performed for this correction, as requested. Compilation and
installation do not establish runtime behavior for every Shell extension.
