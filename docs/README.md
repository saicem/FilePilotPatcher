# Research notes

- [Dialog coverage and Paint investigation](filepilot-dialog-coverage.md) records
  COM/external/WinRT routing, native fallback limits and Paint's read-only metadata.
- [COM replacement and system DLL research](filepilot-com-replacement-and-system-dll-research.md)
  compares activation overrides with replacing Windows DLLs.
- [Native picker UI scheme](filepilot-native-picker-style-scheme.md) maps FilePilot's
  own widgets, drawing and input layers.
- [FilePilot dialogs without an application launcher](filepilot-universal-dialog-scheme.md)
  records the earlier injection-agent design and delivery alternatives.

- [File-operation undo/redo design](filepilot-undo-redo-design.md) maps the 0.8.5
  operation engine and proposes completion-based history with native keybindings.
- `filepilot-combined-patch.md` summarizes the earlier combined patch flow.
- `filepilot-tab-relative-fix.md` records the relative-placement correction.
- `filepilot-tab-relative-placement-design.md` explains the placement design.
- `filepilot-tab-tearoff-analysis.md` documents the original tear-off analysis.

The maintained implementation and current verification record live in
`../patcher/README.md`.

References to the former `work/` scratch directory, one-off patch scripts,
diagnostic logs, screenshots, or superseded executables are historical. Those
temporary artifacts were intentionally omitted from this repository. Current
binary locations and hashes are listed in `../binaries/README.md` and
`../binaries/SHA256SUMS.txt`.
