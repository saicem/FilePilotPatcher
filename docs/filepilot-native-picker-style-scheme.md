# FilePilot native picker: style correction scheme

Investigated 2026-10-09 against the original FilePilot 0.8.5 Ghidra program
and the current picker sources. The correction below is now implemented in
`patcher/dialogs/native_ui.cpp/.h` and compiled into the release picker runtime.
No tests, validation scripts, application launches, or visual
checks were added or run. The screenshot supplied by the user is the observed
rendering evidence.

## Decision

Reconstruct FilePilot's existing control recipes, including their palette
roles, drawing flags, style-stack lifetime, fonts, alignment, rounding, sizing,
and interaction. Calling a native widget helper alone does not supply those
recipes. Replace the footer's generic `button()` abstraction with distinct
action, field, dropdown, and menu-row builders.

The application uses its own immediate-mode widget tree. The traced functions
are FilePilot-specific, not Dear ImGui exports. The current native tree/root
integration can be retained; the immediate defects are in widget construction.

## Why the screenshot looks wrong

| Symptom | Source-level cause | Correction |
| --- | --- | --- |
| Bright green action/type captions | `button()` pushes role 7 as a one-shot. `1d90e0` consumes it for the container and creates a separate `Label` child, which falls back to base role 0. The theme builder explicitly gives role 0 green `(0,1,0,1)` fill/border/text channels. | Use the native dialog action recipe, or give every child its explicit intended role. |
| Invisible action button bodies | `options.flags` contains input and clipping bits, but neither fill nor border. The helper adds `0x6000`, which enables state effects, not shape drawing. | Native dialog actions use `0x406013`: fill, border, text, hover, pressed, left-button interaction. |
| Invisible filename box | `PickerNameFrame` has `0x6000`; the active editor wrapper inherits `0xc00000`. Both omit drawing bits. | Follow the native editor wrapper, including fill/border and its role sequence. |
| Filename/hint/status centred across large rectangles | The code sets child alignment at context `+0x8b8`. Text alignment is a different stack at `+0x980`, whose frame default is `(0.5,0.5)`. | Set text alignment explicitly to `(0,0.5)` for labels, fields, status, and menu captions. |
| Question mark after the type | The caption embeds U+25BE in the text font. Native controls instead use private-use glyphs in the icon font. | Use native arrow glyph `0xe045` with the icon font, in a separate glyph node. |
| No native rounded corners | Frame defaults reset corner radii to zero; our builders never push the control's radii. | Copy native dialog/control radius recipes, using the frame's computed metrics. |
| Focus looks like a held-down button | The generic builder applies `0x10000` for all focused controls. The renderer interprets this as forced pressed state. | Keep focus in picker state; choose a visual recipe appropriate to each control. Native editors and selected menu rows deliberately use this state in specific recipes. |
| Footer appears unusually dark and sidebar divider continues through it | `PickerFooter` has no fill bit, so it is transparent. This lets earlier drawing remain visible. | Paint the native surface across the complete footer, followed by its children, and draw a top separator. |
| Type menu has the same styling defects | Popup uses role 9 with no fill/border and all items use the generic centred action helper. | Use a native popup surface and native menu-row body/caption recipes. |
| Button sizing is fragile for non-ASCII captions | Width is estimated from UTF-8 byte count times 7. | Use native text measurement or text-fit sizing, plus native padding and a minimum action width. |

The transparent footer is confirmed by its missing drawing flag. The exact
browser primitive responsible for the extending sidebar divider was not
individually identified. Painting the footer after the browser roots prevents
earlier decoration showing through; browser geometry should still end at the
reserved boundary.

## Native renderer and style contract

All addresses below are RVAs relative to image base `0x140000000`, scoped to
the existing supported 0.8.5 input. They are not portable across releases.

| Component | RVA / layout | Established behavior |
| --- | --- | --- |
| Context global | `0x249278` | Root list, input state, style stacks, fonts, palette. |
| Theme construction | `0x1974c0` | Builds the live palette from FilePilot's theme configuration each frame. |
| Colour-record allocation | `0x1aaef0` | Copies a `0x70`-byte colour record into the native arena. |
| Palette | context `+0x1870`, role stride `0x30` | Six colour-record pointers: normal, hover, pressed, disabled, disabled-hover, disabled-pressed. Some pointers are null; preserve native fallback behavior. |
| Widget creation | `0x1ca3b0` | Consumes the current role and flags; stores palette-entry pointer at widget `+0x18` and flags at `+0x20`. |
| Widget rendering | `0x1d5080` | Independently draws fill (`1`), outline (`2`), text (`0x10`), and state effects. Colour channels include fill `+0`, outline `+0x10`, text `+0x20`, and alternate foreground `+0x30`. |
| Low-level button | `0x1d90e0` | Container plus optional icon and caption children. It is not a fully styled action-button factory. |
| Native interaction | `0x1d9b20` | Emits the same native mouse signal used by built-in controls. Respect FilePilot's configured press/release activation setting. |
| Native editor | `0x1dd2f0` / `0x1d3600` | Editor tree and mouse selection; outer drawing flags come from its caller. |
| Font-relative padding | `0x1ce050` | Multiplies four padding values by the current font size and rounds to pixels. |
| Spacer | `0x1d1630` | Orientation-aware layout spacer, not a styled button factory. |

Style stack offsets used by these recipes:

| Offset | Meaning |
| --- | --- |
| `0x348` | Palette role |
| `0x4d0` | Inherited widget flags |
| `0x598` | Container layout axis |
| `0x730` | Padding: left, right, top, bottom |
| `0x8b8` | Child layout alignment |
| `0x980` | Text/glyph alignment |
| `0xbe0`, `0xca8` | Width/height rules: pixels, text fit, parent fraction, children fit |
| `0x11e0` | Four corner radii |
| `0x1248` | Native outline thickness |
| `0x1790` | Input layer |
| `0x2e0`, `0x17f8` | Font index and font size |

Each stack has a count and `0x18`-byte entries beginning at `stack+8`.
An entry holds a 16-byte payload and a DWORD at `entry+0x10` controlling
one-shot consumption. `1ca3b0` consumes one-shot styles; container creation
can therefore remove a style before its child is created. Full count/payload
restoration in the existing `Styles` scope is necessary. Add named
`pushOnce` and `pushPersistent` operations rather than modifying the lifetime
word manually at call sites. Restore the complete bank at scope exit.

The base frame entries are intentionally generic: palette role 0, centred text,
zero padding and zero corner radii. Starting from those entries is acceptable
only when each control establishes its entire required recipe. Never assume
the native defaults are already a FilePilot button or field style.

## Control recipes to implement

### Launch/Open/Save and Cancel

Use the built-in modal input dialog at RVA `0x01d6f0` as the reference. Its
`ButtonOK` and `ButtonCancel` construction is particularly direct:

1. Push role `34` (`0x22`) for the next widget.
2. Push four copies of the float radius at context `+0x1888`.
3. Set explicit dimensions, native padding, and centred text at `0x980`.
4. Create one stable-ID widget with `0x406013`, plus the picker positioning
   and clipping flags where required.
5. Copy its caption into the frame arena (`0x212000`) and attach it directly
   at widget `+0x158/+0x160`.
6. Call `0x1d9b20` for interaction and retain the current posted callbacks.

This uses the same theme entry for body and caption, with native hover/press
colours. It avoids the caption-child palette fallback entirely. Keep keyboard
focus separate from mouse press; a focus outline can use an additional native
outline widget if needed, rather than forcing every action into pressed state.

### Filename

Use the modal dialog's field and the editor in RVA `0x158f70` as references.
Both use `1d4c60(7,14,13,10,11,12)`. That helper pushes seven one-shot roles
in the order required by the editor's internal nodes; it is not just one
foreground colour.

The native modal field caller supplies `0xc10003`; the path-editor caller
supplies `0xc00001` and complementary corner radii beside an arrow. Thus
fill, outline, focus treatment and rounding depend on the specific field
recipe. For the standalone filename field, follow the modal field's outlined
recipe. Use role 7 for its outer field rather than inventing a role-9 frame.
Set `0x980=(0,0.5)`, matching inner padding and clipping. Keep the existing
UTF-8 buffer, selection ABI, native mouse selection, keyboard and IME support.

If the inactive field is still rendered as a label, give it exactly the same
outer field geometry and padding as the active editor. Use role 12 for the
hint and role 7 for entered text. Do not introduce a different rectangle or
alignment when editing starts. Apply focused state intentionally here: unlike
the generic action builder, the native modal field itself uses `0x10000`.

### File-type dropdown

Build a field-like composite: left-aligned current type and a trailing native
arrow occupying a fixed icon area. The path-editor arrow in `0x158f70`
provides a confirmed recipe: role 30/31 selected by context, persistent theme
around `1d90e0`, glyph `0xe045`, and icon font from context `+0x1938`.
The text font index is separately at `+0x1934`.

Reuse that arrow/body pattern, with explicit fill/outline on the dropdown
body, and an explicit text role for the caption. Its hit target is the complete
dropdown rectangle. Truncate long type captions inside that rectangle while
retaining the trailing arrow area. Do not append symbols to the UTF-8 caption.

### Popup and selectable rows

Use a filled native popup surface with outline and native radii. Native menu
rendering at `0x1beed0` establishes separate roles:

- Row body: role 68 (`0x44`), native flags `0x440001` for an ordinary row and
  `0x450001` for its highlighted/selected state.
- Caption: role 32 (`0x20`), text flag `0x10`.
- Secondary text: role 33 (`0x21`), where applicable.

Replicate those body/caption constructions rather than styling options as
action buttons. Keep captions left-aligned and vertically centred; use a
single stable row ID for interaction. Use a raised input layer and a separate
full-window popup root, so rows over the browser or filename field can receive
clicks. Popup placement remains bounded by available browser space.

The row-state flags above deliberately use the native forced-state behavior.
They are a menu selection recipe, not a universal keyboard-focus property.

### Labels, status and optional read-only control

Use explicit normal/muted roles and left-aligned text for field labels and
status. Render status as a secondary line aligned with the input column,
with space reserved when the optional read-only control is present.

The native toggle was subsequently traced in `1c7bd0`: role 78 (`0x4e`),
fill/outline flags `3`, small corner radius at context `+0x1884`, and checked
glyph `0xe01c` in the native icon font. The implementation uses this indicator
recipe with a separate caption and a full-row native mouse target. The glyph
is a child of the indicator so the indicator's fill cannot cover it during
FilePilot's reverse sibling draw order. Unicode checkbox symbols were removed.

## Footer geometry, scale and state

Keep the dedicated interactive root and browser-height reservation. The final
renderer `1cc6c0` visits root slots in order. After construction/layout, the
`1d5080` render hook paints the footer before the first native root and skips
its later scheduled draw. Native context menus are built inside the browser
tree after their temporary measurement root is removed; painting the footer
after the browser root would still cover them. Preserve native root-slot order
and cached widget root indices, changing only the footer's draw timing.
Give its footer a fill using the existing native surface role 26 and a thin
native top separator. The type popup uses a separate last root: sibling drawing
inside the footer root is reversed, so a sibling menu could be covered by the
footer's children. Root paint order is independent of the input-layer stack.

Footer widgets remain at input layer 0, while the type popup uses layer 1 and
raises the frame's input floor. Mouse routing checks the popup rectangle before
footer rectangles; an outside press dismisses the menu without activating the
underlying control. Existing native popups retain keyboard ownership. Automatic
filename focus selects the basename before its final nonempty extension;
dotfiles and extensionless names retain their entire basename selection.
Count filename presses in the HWND subclass using Windows' double-click time
and position tolerances. On the second press, select the basename when clicking
the title or the extension letters when clicking the final suffix. Determine
the suffix hit area using the filename's native font measurement and last
painted text origin/scroll; exclude the dot from the selected range. Apply
whole-field selection on the third, before highlight construction and after
native mouse handling. Retain the range while held and through release; cancel
the override when dragging or editing with the keyboard. This avoids relying
on the native widget's capture-dependent click counter. Explicit Ctrl+A still
selects the complete field. Single-click placement and dragging retain native
capture, with caret offsets recomputed from the painted text origin and native
glyph advances. The native helper uses the outer field's left edge, which
otherwise shifts the padded footer editor's caret to the right.

Painting the footer first exposes the sidebar's full-height decoration. The
previous clamp targeted its resize strip, but the sidebar builder (`1accd0`)
constructs the visible line and viewport separately. Reserve the footer in
renderer `+0x48` during that builder, where pass zero reads its explicit height,
and restore the renderer immediately afterward. This gives all sidebar nodes
the available browser height at construction. The UI context remains full
height so native menus can still extend over the footer.

Build two rows with a shared label column: filename and file type. Align the
type dropdown and the action pair on the second row. Use native font-relative
padding and gaps. Calculate the reserved height from the same row metrics used
to draw the footer so browser reservation, paint geometry and hit rectangles
cannot diverge. This also removes the current half-window height cap versus
fixed internal-row-position mismatch in very short windows.

Use native text-fit sizing or font measurement (`1b9d10`) for action captions;
avoid UTF-8 byte-count estimates. Retain a minimum comfortable action width.
Read FilePilot's already computed corner/outline metrics (`+0x1884`,
`+0x1888`, `+0x1894`) so its rounding preference and font scaling carry through.
Do not apply a second Windows DPI multiplier to those pixel values.

Continue to use stable picker IDs, native interaction, press/release preference,
the current dropdown input layer, and posted session actions. Treat hover,
mouse press, keyboard focus, menu highlight and disabled state as distinct
inputs to each control recipe.

## Implementation scope and order

1. In `patcher/dialogs/native_ui.cpp/.h`, introduce named flags, role constants,
   lifetime-aware style operations and explicit text alignment/radius helpers.
2. Replace action construction with the native modal-dialog recipe. Give the
   footer a native fill and separator. These changes address the green captions,
   invisible buttons, and transparent footer directly.
3. Rebuild filename rendering from the standalone native field recipe, keeping
   active and inactive geometry consistent.
4. Implement the type composite and native menu rows, removing text-font symbols.
5. Derive footer reservation and hit rectangles from shared native metrics;
   replace byte-count caption sizing. The optional toggle recipe has also been
   extracted and its native indicator construction applied.
6. Rebuild and distribute the picker runtime DLL. The launcher, adapter protocol,
   application hooks, and dedicated FilePilot helper executable need no changes
   for the established style defects.

Do not modify the live palette to hide role-0 fallback: it is shared by all
native controls. Read palette entries through the current context each frame;
theme construction allocates their colour records in the native arena.

No additional Windows binaries are required in Ghidra for this work. All
identified style behavior is within the already analyzed FilePilot executable.
Visual parity and runtime behavior are not claimed by this investigation.

## Evidence files

- `analysis/dialogs-native-ui/native-render.txt`: renderer drawing bits, state
  mixing, palette channels, text alignment and recursive drawing.
- `analysis/dialogs-native-ui/theme-recipes.txt`: live theme construction,
  green role 0, palette assignments, and native menu row/caption recipes.
- `analysis/dialogs-native-ui/palette-dialog.txt`: native modal field and
  `ButtonOK`/`ButtonCancel` constructions.
- `analysis/dialogs-native-ui/control-recipes.txt`: path editor, arrow, padding,
  corner treatment, and persistent theme examples.
- `analysis/dialogs-native-ui/button-layout.txt`: low-level button's separate
  container/icon/caption construction.
- `analysis/dialogs-native-ui/widgets.txt`: editor internals, padding conversion
  and interaction.
- `analysis/dialogs-native-ui/lifecycle.txt`: generic frame defaults.
- `analysis/dialogs-native-ui/ui-frame.txt`: style consumption and widget layout.
- `analysis/dialogs-native-ui/panel-geometry.txt`: root render order.
- `analysis/dialogs-native-ui/layout-owner.txt`: frame metrics and theme setup.
- Current defect sites: `patcher/dialogs/native_ui.cpp` lines 200-350.

