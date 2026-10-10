# Picker filename selection and popup order

The user screenshot shows the entire filename selected and type/context menus
covered by the bottom picker controls. This correction stays in the separate
FilePilot picker UI runtime; the current per-user COM replacement is retained.

## Static findings

`focusName(true)` unconditionally selected every UTF-8 byte in the field. Save
initialization, choosing an existing file in Save mode and Tab focus all call
it. Native mouse placement, dragging and word selection are separate editor
operations at RVA `0x1d3600`; explicit Ctrl+A is handled separately as well.

Ghidra exports `analysis/dialogs-native-ui/panel-geometry.txt` show that final
rendering (`0x1cc6c0`) visits the root slots in ascending order. The root count
is at context `+0xec`; each slot at `+0xf0` has size `0x20`. Input layer
(`0x1790`) controls interaction eligibility, not draw order. Root begin
(`0x1ce3e0`) appends a slot; its first argument controls measurement skipping.
Root end (`0x1cebd0`) computes layout and retains the slot for rendering.

The footer was appended after the browser root, covering popup lower parts.
The native popup builder (`0x3e860`, `roots.txt`) uses a temporary measurement
root, removes that root, then creates the interactive popup inside the current
browser scope. Reordering the footer after the primary root therefore cannot
put it below these menus. Native widget creation also caches each widget's
root-slot index, so the correction must avoid reordering the actual slot table.
The type menu was attached to the same root as the footer. FilePilot's reverse
sibling traversal could then paint footer children over that menu. Mouse
routing also checked the filename before the overlapping type-menu rectangle.

## Change

Automatic filename selection now starts after the last directory separator
and stops before the final nonempty extension. A leading dot is treated as
part of the basename. Extensionless names and names ending in a dot select
their whole basename; multiple suffixes preserve the last suffix. Selection
uses UTF-8 byte offsets, matching the native text ABI. The previous attempt
depended on the click count returned by native mouse handling (`0x1d3600`);
the user reported that both double/triple-click behavior and the divider were
unchanged. Filename clicks are now counted at the window subclass using
`WM_LBUTTONDOWN`/`WM_LBUTTONDBLCLK`, `GetMessageTime`, the Windows double-click
interval and its position tolerance. The second press selects the basename
when clicking the title, or the extension letters when clicking the final
extension (without selecting the separating dot). The third selects the
complete field. Extension hit detection measures UTF-8 prefixes with the
filename renderer's font and uses the last painted `ScrollContents` origin
and animated horizontal scroll (`+0x208`), so scrolling and variable-width
characters do not move the hit boundary away from the visible suffix.
Leading-dot names and trailing dots have no separate extension selection.
Selection is applied before the native
editor constructs its highlight and after native mouse handling, and retained
while the press is held. Release queues a final application. A drag cancels
the override, as do focus loss, cancellation, keyboard editing and clicking
another control. Single-click placement, dragging and Ctrl+A retain their
capture/keyboard behavior. Native popups retain input priority.

The native single-click/drag handler measures mouse X from the outer field's
painted left (`widget+0x290`), without subtracting the field's text padding.
The footer has an inset, so this maps the pointer to a later character.
After native capture handling, that single-click/drag range is now recomputed
from the painted text origin (with a field-padding fallback on first render).
The press anchor uses native capture X and its captured scroll (`ui+0x80/0x88`);
the moving caret uses current mouse X and `ScrollContents+0x208`. A linear
UTF-8 glyph walk uses the renderer's font and the nearest painted caret
boundary, preserving variable-width characters. Double/triple-click range
overrides run afterward. This adjustment stays in the picker filename field.

Build and lay out the footer and type menu in separate interactive roots.
The render hook (`0x1d5080`) draws the completed footer once, immediately before
the first root-render call in native finalization, then skips its later scheduled
draw. A recursion-depth guard distinguishes root passes from widget-child calls.
The browser tree, including its embedded context menus, is painted afterward;
the type menu remains last. Browser geometry already reserves the footer area.
Full-window type-menu geometry avoids footer clipping. Root count, slot order,
widget root indices, native layout, cache cleanup and finalization are retained.

The earlier paint-time clamp identified the sidebar resize strip with the
kind-1 view's `+0x6f0` seed followed by `Border`. Further decompilation of the
sidebar builder (`0x1accd0`, `mouse-and-sidebar.txt`) shows that it separately
constructs the visible line and sidebar viewport. Pass zero reads the full
window height from the renderer at `+0x48` and uses it for explicit geometry;
shortening the strip does not relayout the other nodes. That clamp is removed.
The picker now reserves the footer at this shared height source while calling
the sidebar builder, restoring it immediately afterward. The browser panels
already apply the same reservation at their rectangle source. All sidebar
geometry is therefore constructed with the available browser height. The UI
context retains its full height for popup placement. No global popup clipping
or fixed sidebar width is introduced.

Footer controls use input layer 0; the type menu uses layer 1 and raises the
input floor. Its hit rectangle takes precedence over overlapping footer
rectangles. Outside presses dismiss it without activating a covered control.
An already active native popup takes precedence over the type menu.

## Delivery

Compile production targets with `patcher/dialogs/build-com.ps1` and install
the resulting immutable payload through `install-com.ps1 -Action Install`.
Close existing picker windows and fully reopen caller applications to use the
new COM factory path. No system DLL replacement or additional application
injection is introduced.

Production compilation completed and the installer upgraded both x64 and x86
per-user registrations on 2026-10-10 to
`ComRuntime-1-20261010-060103-2038091`. Existing registration backups and
installer ownership were retained. No caller application was terminated.

Following the user's report that double-click and the native context menu
still used the old behavior, the explicit double-click handling and render-pass
ordering above were compiled and installed in
`ComRuntime-1-20261010-060713-5179600`, updating both per-user registrations.

The sidebar boundary correction and explicit triple-click selection were
compiled and installed in `ComRuntime-1-20261010-062223-5714789` through the
same x64/x86 per-user COM upgrade. The user reported no visible change. The
current correction replaces that paint-time clamp and native-signal click
dependency with the construction-time reservation and HWND click tracking
described above.

Production compilation completed for this correction and the per-user COM
installer upgraded both architectures to
`ComRuntime-1-20261010-063036-7638082` on 2026-10-10.

Extension-aware double-click selection was compiled and installed through the
same per-user COM upgrade in `ComRuntime-1-20261010-063415-4009686`.

The filename caret-origin correction was compiled and installed through the
same per-user COM upgrade in `ComRuntime-1-20261010-063821-6549511`.

No tests, validations, visual checks or application-dialog trials are written
or run. Compilation and installation alone do not establish runtime results.
