# Preventing native Open/Save dialog flashes

Researched 2026-10-10 from the implementation and primary Windows documentation.
No application dialogs were opened or manipulated, no tests or validations were
written or run, and no interception mode or installed executable was changed.

## Conclusion

For supported dialog requests, FilePilot can replace the native UI before it
appears by intercepting the API request in the requesting process. The existing
adapter already implements this architecture. Automatic agent delivery removes
the need for a specialized application launcher, but does not remove the
in-process adapter.

The current fully external watcher cannot promise that no native frame is ever
visible. Its notifications arrive asynchronously after an event, and hiding is
also asynchronous. Faster recognition can shorten the exposure substantially;
it does not establish a pre-display barrier. This conclusion follows from the
documented [out-of-context event delivery contract](https://learn.microsoft.com/en-us/windows/win32/winauto/out-of-context-hook-functions)
and [ShowWindowAsync behavior](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindowasync).

## Why the current delay can last seconds

The installed source follows this sequence:

1. `external_agent.cpp`: receive SHOW/foreground notification, post another
   message to the controller, and check the candidate. Missed notifications are
   covered by a 500 ms sweep. Candidates must already be visible and enabled.
2. Create a worker process and initialize COM/UI Automation there.
3. `external_dialog.cpp`: read fixed native control IDs; query individual host
   accessibility elements; retrieve the root descendant accessibility tree;
   inspect options, actions, folder, and file types.
4. Only after `NativeDialog::inspect` succeeds, enter `Phase::Picker` and request
   hiding the native dialog, then launch FilePilot.

The previous change moved hiding ahead of FilePilot startup, but left it after
the complete recognition pass. UI Automation connection and transaction timeouts
are configured at 1,500 ms. Several provider calls are involved. The 2,500 ms
readiness loop is a retry deadline, not a hard bound on an individual inspection;
an inspection may run past it. The worker watchdog is separate.

This source ordering explains how seconds of exposure are possible. It is not a
runtime measurement identifying the precise duration of each call on this machine.
Once requested, `ShowWindowAsync` posts work to the target window's message queue;
it does not wait for hiding to complete. A successful return means the operation
was started, not that the window has already disappeared. [Microsoft reference](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindowasync).

## Mechanisms compared

| Mechanism | Prevents every initial frame? | Relationship to application |
| --- | --- | --- |
| Current external events plus hide | No guarantee | Entirely external |
| Earlier CREATE events, fast recognition, prestarted workers | No guarantee; reduces delay | Entirely external |
| DWM cloak applied after external detection | No guarantee; detection still races display | Cross-process permissions must not be assumed |
| In-context event callback | Earlier, synchronous callback; not a universal show veto | DLL executes in application |
| CBT creation hook plus controlled visibility | Possible design for identified native dialogs; requires more implementation | Normal matching-architecture route executes in application |
| Replace supported Open/Save API request before native display | Native picker is never launched on the replacement path | In-process adapter, external FilePilot UI |

### CREATE notifications and cloaking

Subscribing to `EVENT_OBJECT_CREATE` can provide earlier discovery than SHOW.
However, an out-of-context callback still uses queued delivery. A CREATE event
is a notification about creation, not a supported cancellation callback. Early
classification also has less information than a fully initialized dialog.
[Event definitions](https://learn.microsoft.com/en-us/windows/win32/winauto/event-constants),
[delivery contract](https://learn.microsoft.com/en-us/windows/win32/winauto/out-of-context-hook-functions).

`DWMWA_CLOAK` makes a window invisible while DWM continues composing it. That is
potentially useful for keeping a native dialog initialized and logically alive.
It is not a subscription that automatically cloaks future file dialogs. Applying
it after our external notification leaves the same initial race. The public
attribute documentation does not establish that this ordinary external agent
may cloak arbitrary other-process windows; that should not be promised without
additional evidence. No cloak experiment was performed. An in-process caller
would avoid depending on arbitrary cross-process control. [DWM attributes](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute),
[attribute setter contract](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmsetwindowattribute).

### CBT and message hooks

`WH_CBT` receives `HCBT_CREATEWND` before `WM_NCCREATE`/`WM_CREATE`, when the new
HWND exists but its parent and geometry may still be incomplete. Returning
nonzero destroys that window and causes `CreateWindow` to fail. That is not a
replacement result for the application's Open/Save operation. `HCBT_ACTIVATE`
can veto activation, but activation and visibility are different operations;
vetoing activation alone does not establish suppression. [CBT contract](https://learn.microsoft.com/en-us/windows/win32/winmsg/cbtproc).

A creation-stage design must let the native operation initialize, identify it
as a file dialog, and gate visibility until recognition/picker handoff completes.
`WM_WINDOWPOSCHANGING` permits the window procedure to adjust the pending
`WINDOWPOS` flags, but `WH_CALLWNDPROC` itself is documented as observation only.
Merely adding that message hook does not create a show veto. A local procedure
or equivalent local interception is needed. [Window-position message](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanging),
[message-hook limitations](https://learn.microsoft.com/en-us/windows/win32/winmsg/callwndproc).

Global `SetWindowsHookEx` removes manual per-application launching, but normally
loads a matching-architecture DLL into eligible applications. x86/x64 deployment
and a responsive host message loop are required. Microsoft also documents
opposite-bitness callbacks running in the installing process. That exception
must not be mislabeled as proof that every hook always injects. It nevertheless
does not document a universal file-dialog-specific visibility gate, and the
same hook can inject into matching-bitness applications. I have not established
this as a reliable entirely external suppression architecture. [Hook deployment](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw).

In-context WinEvent callbacks also load into the application. Some events can
still be delivered out of process. Process protection policies can force event
hooks back to external delivery. A design cannot assume synchronous local
execution in every application. [In-context delivery](https://learn.microsoft.com/en-us/windows/win32/winauto/in-context-hook-functions),
[extension-point mitigations](https://learn.microsoft.com/en-us/defender-endpoint/exploit-protection-reference#disable-extension-points).

## Recommended implementation scheme

For the specific requirement that a supported native picker never appears,
prefer the existing API adapter over a new generic dialog-hiding hook:

1. Reuse `Dialog::Show` in `adapter.cpp`. Its supported branch calls `runPicker`
   and does not call the native `Show`. Modern dialog display is controlled by
   the modal Show call. [Show contract](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-imodalwindow-show).
2. Reuse the legacy W/A `GetOpenFileName` / `GetSaveFileName` adapters. They return
   FilePilot's selection in the application's existing result contract rather
   than starting a native picker on supported requests. [Legacy Save API](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/nf-commdlg-getsavefilenamew).
3. Deliver the adapter automatically through the resident agent before dialog
   objects/factories are created. For the previously investigated Chromium
   UtilWin path, retain the existing scoped child-process delivery before the
   utility child resumes. Attaching only after its dialog appears cannot undo
   the first visible frame. The current bootstrap only requests asynchronous
   discovery; it is not itself an early display gate.
4. Treat adapter installation and supported request routing as separate states.
   Unsupported customization/options, already-created objects, blocked adapter
   delivery, and application-drawn pickers do not acquire guaranteed suppression.
5. Retain external interception as an optional fallback with faster early
   recognition, not as the guarantee for every request. Current agent/external
   modes deliberately exclude simultaneous operation; a combined package would
   need explicit routing and arbitration before enabling both.

If keeping all application processes untouched takes priority, improve the
external mode instead: split recognition into a fast native file-dialog gate
and slower metadata extraction; hide a positively identified candidate before
COM/UIA work; prestart workers; keep reliable restore-on-failure behavior. The
recognizer must be adapted for hidden windows: its current `IsWindowVisible`
checks and offscreen accessibility checks depend on visible ancestors. Hiding
all `#32770` windows is unsuitable because that class also includes unrelated
dialogs. This option can remove the current recognition delay from the visible
interval, but should be described as reducing flashes, not eliminating them.

No deployment changes were made during this research. No additional Windows
binaries or Ghidra imports are needed for the recommended API-adapter approach.

## Subsequent implementation

The user subsequently requested API interception before native display. The
existing modern COM and legacy W/A adapters were reused. The API agent installer
now gracefully stops this package's external watcher before installing its
sign-in entry; it checks controller executable identity, waits for normal
shutdown, and declines switching if shutdown fails or another package owns the
watcher. Controller command constants are shared with the external mode.

Production x64/x86 payloads were built and packaged, the external watcher was
stopped, and `FPilotDialogAgent.exe --install` was launched. An already running
browser needs one full close/reopen to load the selected adapter and early
utility-child delivery code. No applications were closed automatically. No tests
or runtime validations were written or run. Unsupported requests and delivery
failures retain native fallback; no universal first-request guarantee is added
for uninstrumented applications.
