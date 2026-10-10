# Unofficial Project
> This is an independent, unofficial project and is **not affiliated with, endorsed by, or associated with the official File Pilot developers**. Please do not contact the official developers for support related to this project.
# File Pilot reverse-engineering project

This repository contains the Ghidra analysis, patcher, regression fixtures, and
research notes for File Pilot 0.8.2/0.8.5 modifications and the native Open/Save
dialog replacement.

## Repository layout

- `patcher/` - maintained patcher source, payload, build wrapper, and tests.
- `ghidra/project/` - the current Ghidra project and analysis database.
- `ghidra/scripts/` - FilePilot-specific Ghidra automation.
- `binaries/input/` - the unmodified File Pilot 0.8.5 patch input.
- `binaries/regression/` - known builds used by regression tests.
- `binaries/release/` - the current combined patched executable.
- `docs/` - design notes and historical implementation documentation.

Executable files and Ghidra database blobs are stored with Git LFS. After
cloning, run:

```powershell
git lfs pull
```

## Build and test

Install the Python dependencies, build from an x64 Visual Studio toolchain,
and run the regression suite:

```powershell
python -m pip install lief capstone
Set-Location .\patcher
& .\build_patch.ps1
python -m unittest -v .\test_locator.py
```

The default build reads `binaries/input/FPilot-0.8.5.exe` and writes
`binaries/release/FPilot-0.8.5-open-location-tab-merge.exe`.

To build every maintained patch, use the combined profile:

```powershell
& .\patcher\build_patch.ps1 -All
```

This writes
`binaries/release/FPilot-0.8.5-all-patches.exe` and its JSON manifest. It
contains Open File Location, tab creation/tear-off, cross-window tab merge, the
fixed native-row Unicode renderer, and the native context-menu cache. DirectWrite
shapes and rasterizes extended text into cached immutable R8 resources, then File Pilot emits them as ordinary
native type-0 textured quads. The Unicode hook calls the verified native quad
emitter directly, so there is no text-renderer bridge, carrier, marker, frame hook, draw-batch hook,
or custom graphics pipeline. Stable cached commands preserve File Pilot's native
unchanged-frame short circuit when a folder is idle.

See [docs/filepilot-unicode-patch.md](docs/filepilot-unicode-patch.md) for the
renderer design, patched regions, and current limits.

The menu cache retains a partitioned LRU of complete native Shell menu wrappers:
64 selected-item contexts and four folder-background contexts. On a miss it
builds the replacement while existing Shell interfaces remain alive, then
defers any eviction until the new menu closes. Folder, selection, custom context
entries, query flags, and modifier state form each key; clipboard sequence is
included only for background menus, whose Paste state depends on it. This avoids repeating synchronous
`IContextMenu::QueryContextMenu` enumeration for recent contexts and prevents
Shell teardown from entering the right-click open path. See
[docs/filepilot-context-menu-cache.md](docs/filepilot-context-menu-cache.md).

## Open and Save As dialogs

Build the dedicated FilePilot 0.8.5 picker and x64/x86 modern-dialog COM bridges:

```powershell
& .\patcher\build_patch.ps1 -DialogsOnly
# Include the optional Windows Runtime provider:
& .\patcher\build_patch.ps1 -DialogsOnly -WinRT
```

The `-All` profile also builds the COM/external dialog package alongside the
patched executable. Builds do not install providers or run tests. Install the
generated package separately using `binaries/release/dialogs/install-com.ps1`;
WinRT registration uses its separate elevated installer. COM and WinRT use
normal DLL loading; the external fallback observes native windows after creation
and can still flash. See [dialog instructions](patcher/dialogs/README.md) and
[coverage analysis](docs/filepilot-dialog-coverage.md).

## Ghidra

Open `ghidra/project/FilePilot.gpr` in Ghidra. See `ghidra/README.md` before
committing analysis changes.

## Binary provenance

Expected hashes are recorded in `binaries/SHA256SUMS.txt`. The File Pilot
executables remain subject to their original vendor license; review that
license before distributing a clone or release bundle.
