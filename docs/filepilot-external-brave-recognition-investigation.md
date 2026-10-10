# Why Brave's Save dialog remains native in external mode

Investigated 2026-10-10 from the user's already-open dialog, existing process and
shared diagnostic state, and interceptor source. No dialog was opened, save
initiated, control changed, test written/run, or runtime validation performed.

## Confirmed failure stage

The external watcher was running as PID 28164; the injection agent was absent.
External mode was enabled and the exclusions file contained only comments.
The watcher's existing `ExternalJob.28164.2` mapping identified Brave's UtilWin
process PID 24192, thread 31752, and native dialog HWND `0x390dc4`.
Its worker PID 31324 exited with Windows error 50 (`ERROR_NOT_SUPPORTED`).
The job remained in `Phase::Inspecting`, with `stop=0` and `hidden=0`. The native
window was visible and enabled, and still carried the worker's claim property.
The failure precedes `runPicker` and is not the earlier Chromium attachment race
or the FilePilot `OnFileOk` rejection. Process discovery reached this dialog.

## Control contract actually exposed on this machine

Read-only Win32 descendant metadata and UI Automation properties exposed:

| Role | Native control | Win32 ID | Direct UI Automation element |
| --- | --- | --- | --- |
| Filename | `ComboBox` with an `Edit` child | Combo `0`, edit `1001` | Pane / `AppControlHost`, ID `FileNameControlHost` |
| File type | Dropdown-list `ComboBox` | `0` | Pane / `AppControlHost`, ID `FileTypeControlHost` |
| Save | `Button`, caption `&Save` | `1` | Pane, ID `1` |
| Cancel | `Button` | `2` | Pane, ID `2` |
| Current folder | Address `ToolbarWindow32` | `1001` | Pane; name contains the absolute current directory |

The direct filename/type elements report no supported UI Automation patterns.
A descendant accessibility snapshot of the root dialog includes Shell file-view
and search controls, but omits the filename/type hosts and native action buttons.
The native filename editor, type selector, and action buttons themselves exist
and their text can be read through bounded standard Windows messages.

## Why the recognizer rejects it

`NativeDialog::inspect` first locates the filename through legacy Win32 IDs
`edt1` (1152) or `cmb13` (1148), and the type selector through `cmb14` (1149)
or `cmb1` (1136). Those IDs do not match this dialog's ID-0 combo boxes.

Its accessibility fallback then requires Edit/ComboBox control types with those
numeric automation IDs. The observed host panes have semantic string IDs and
are not returned in the root's descendant snapshot. Consequently no filename
editor or type selector is assigned, and the required-controls check returns
false. The visible Shell view and Save/Cancel buttons are insufficient to pass.

After the worker exits, the watcher keeps the native window's claim property.
It deliberately does not retry the same window, so this dialog stays native
until it closes. That behavior prevents repeated interception attempts but makes
the missing recognition support persistent for the current dialog.

## Concrete correction scheme

1. Enumerate native descendants in addition to the root accessibility tree.
   Query each relevant combo's direct accessibility element for semantic host
   IDs, regardless of its reported ControlType or pattern availability.
2. Recognize `FileNameControlHost` and `FileTypeControlHost`. Bind the filename
   host to its actual native Edit child, and bind the type host to its native
   dropdown-list ComboBox. Keep legacy numeric-ID support. Require an existing
   Shell file view and recognizable native action buttons to avoid claiming
   unrelated dialogs with ID-0 controls.
3. Use bounded Win32 filename/type messages and native action delivery for these
   controls; their missing accessibility patterns do not prevent that path.
   Read the absolute folder from the native address toolbar instead of falling
   back to Documents when the root accessibility tree omits that toolbar.
4. Record the precise recognition stage/reason in existing per-job diagnostics
   instead of returning only the generic unsupported state.

## Implemented correction

The external recognizer now queries native combo handles directly and recognizes
the two semantic host IDs regardless of accessibility ControlType. It binds the
filename host's Edit child and the type host's dropdown-list ComboBox, retaining
legacy numeric-ID support and the surrounding file-view/action checks. Native
address toolbar text supplies the initial directory. Existing bounded Windows
messages transfer the selection back through the original native dialog.

Per-job diagnostics now retain recognition, picker, and submission failure
reasons with available error codes; the controller displays them in status.
The production external executable is rebuilt and packaged for deployment.
No tests or runtime validations are written or run; compilation alone does not
establish whether Brave accepts an actual save through this updated path.
