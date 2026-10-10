# Brave download Save As investigation

## Confirmed cause after the browser restart

The follow-up investigation on 2026-10-10 establishes that delivery is too late
in a newly created Chromium Windows utility process. Attaching the main browser
does not intercept the utility process's COM activation. The previous investigation
traced the generic Windows dialog implementation but missed Chrome's factory
that selects out-of-process execution.

The user restarted Brave, then provided a screenshot and left their native prompt
open. Read-only inspection found:

| Evidence | Observation |
| --- | --- |
| Main browser | brave.exe PID 36644, updated adapter loaded, enabled, no intercepted request |
| Prompt process | brave.exe PID 17724, parent 36644, `--type=utility --utility-sub-type=chrome.mojom.UtilWin --service-sandbox-type=none --message-loop-type-ui` |
| Prompt window | HWND 0x1207c6, class #32770, thread 28936 in PID 17724, owned by a browser window |
| Utility attachment | Host reported Attached with Windows error 0; the selected updated adapter and bootstrap were loaded |
| Utility COM entry | The resolved Save-dialog CoCreateInstance import pointed to the patched combase function, whose relay led to the updated adapter |
| Utility policy/record | Managed flag 1; diagnostics `(version=2, bytes=20, sequence=0, route=None, detail=0)` |
| Existing dialog thread stack | Included chrome.dll return RVAs 0x0c56361e and 0x0b961a8b |

Static disassembly identifies 0x0c56361e as the return address after the virtual
IFileDialog::Show call (vtable slot +0x18) in the installed Save-dialog body.
The enclosing utility caller uses the `Windows.TimeInSelectFileDialog` metric,
matching UtilWinImpl::CallExecuteSelectFile. Only matched code addresses were
reported from a bounded read of the existing thread's stack; no debugger was
attached, thread suspended, UI clicked, or new download initiated by the agent.

The native object was already created and its Show was running before the adapter
installed its creation hooks. The hooks currently present cannot convert that
existing object. This is the observed first-request race in the utility process,
not an unsupported-option fallback recorded by FilePilot. Reopening a prompt
can launch another fresh utility process and repeat the same race.

### Source corroboration

Chrome's [dialog factory](https://github.com/chromium/chromium/blob/main/chrome/browser/win/chrome_select_file_dialog_factory.cc)
constructs UtilWinHelper, launches a UtilWin service instance, and sends
CallExecuteSelectFile over Mojo. The
[service launcher](https://github.com/chromium/chromium/blob/main/chrome/browser/win/util_win_service.cc)
requests a UI message loop; the
[service interface](https://github.com/chromium/chromium/blob/main/chrome/services/util_win/public/mojom/util_win.mojom)
uses kNoSandbox. The
[utility implementation](https://github.com/chromium/chromium/blob/main/chrome/services/util_win/util_win_impl.cc)
calls the generic Windows dialog implementation inside the utility process.
The live command line and installed binary agree with these primary sources.

### Why the current delivery cannot fix it

`agent_bootstrap.cpp` only posts a discovery message from WH_GETMESSAGE. It
does not install dialog hooks synchronously. `agent_host.cpp` installs them on
its worker after discovery, supplemented by a two-second sweep. A UI utility can
create its native COM dialog before its first relevant message-hook callback;
even a callback delivered earlier only queues asynchronous attachment. Sorting
the foreground process first does not order utility dialog creation after
attachment. Restarting the main browser does not impose that ordering either.

### Required implementation

Add parent-process delivery to the adapter already resident in the browser.
Intercept the actual child-creation path and identify the intended unsandboxed
UtilWin child. Arrange child adapter initialization before application code can
activate FileSaveDialog, then allow execution to continue. Use a maintained
pre-entry delivery mechanism, preserve the caller's original suspension state,
handles, startup attributes, environment, error behavior and sandbox policy, and
continue the child with native dialogs if bounded initialization fails. Do not
reuse the running-process attachment routine against an uninitialized suspended
child without explicit loader initialization/pre-entry handling.

Chromium's [ordinary Windows process launcher](https://github.com/chromium/chromium/blob/main/base/process/launch_win.cc)
uses CreateProcess or CreateProcessAsUser, depending on launch options; cover
the actual call path rather than assuming one export is sufficient. Initially
scope early delivery to same-user, same-architecture UtilWin children with the
observed no-sandbox configuration. Keep asynchronous discovery for other processes.
This approach preserves normal browser shortcuts and requires no special launcher.

At the end of the investigation this parent component was missing. It has since
been implemented after the user's explicit instruction to proceed, as described
below. Faster polling alone cannot provide the required ordering.

### Implementation delivered afterward

`child_delivery.cpp` adds CreateProcessW/CreateProcessAsUserW hooks and distinct
kernelbase-export coverage to the agent adapter. Exact UtilWin/no-sandbox switches
and the browser's own executable identify candidate children. Actual executable,
architecture, user/session, token and mitigation restrictions are checked against
the created child before initialization. The original main thread stays suspended
until loader initialization and FilePilotAgentAttach complete. The child continues
on failure, and any suspension requested by the caller remains in place.

`attachment.cpp` extracts the existing launcher's pre-entry loader approach into
separate suspended-child handling. PID/creation-time mutexes serialize parent and
discovery attachment. Timed-out remote parameter allocations remain alive. The
parent's early-delivery record is independent of the child's dialog-routing record;
the agent displays both. The handshake includes a child-delivery hook capability.

The x64/x86 production targets compiled. No tests, runtime trials, or validations
were written/run. The agent package is updated and restarted for deployment; the
browser must restart normally once to load the new parent adapter. This is compiled
implementation, not evidence of successful dialog behavior in a new request.

---

## Save selection rejection after early delivery

The user's subsequent screenshot shows the FilePilot picker with the footer
message "The application rejected this file selection." This message originates
in the adapter's `OnFileOk` sink loop, after the selected path has passed the
picker's existing path checks and has been represented as a shell item.

The loop previously required every sink to return `S_OK`. Chromium registers an
[auto-close sink](https://github.com/chromium/chromium/blob/main/ui/shell_dialogs/auto_close_dialog_event_handler_win.cc)
whose `OnFileOk` implementation returns `E_NOTIMPL`. That handler only manages
owner destruction; this return value does not reject a filename. The adapter
now treats `E_NOTIMPL` as an unimplemented event, as it already did for
`OnFolderChanging`. Explicit `S_FALSE` vetoes and other callback errors still
keep the picker open. No event sink is removed or bypassed.

The helper now also sends the initial `OnTypeChange` event after publishing its
visible window, rather than waiting for the user to change file type. Chromium's
auto-close and
[safe-accept sinks](https://github.com/chromium/chromium/blob/main/ui/shell_dialogs/safe_accept_file_dialog_event_handler_win.cc)
initialize from that event. This preserves their lifecycle initialization and
allows `IOleWindow::GetWindow` to return the ready helper window during the callback.

This diagnosis is based on the reported error location and source inspection.
No download or save is initiated to reproduce it, and no tests or validations
are added or run. Compilation and package deployment are separate from runtime
confirmation of a successful save.

---

## Earlier investigation, before utility ownership was observed

Observed on 2026-10-10. The reported action is downloading a file in Brave.
No dialogs were opened, downloads initiated, tests written/run, or runtime
validations performed for this investigation.

## Evidence

The resident agent and both attachment hosts were running and replacement was
enabled. The x64 host reported successful attachment to Brave's existing browser
process. Its loaded modules included the selected runtime's FPilotDialogs.dll.
The adapter's managed flag was enabled and its diagnostics mapping was present.
The mapping had no intercepted request recorded at the time of inspection.

Static inspection of the installed Brave 155.1.97.56 chrome.dll found the
FileSaveDialog activation using IID_IFileSaveDialog and CLSCTX_INPROC_SERVER.
Reading that call's resolved import slot in the existing browser process showed
it pointing to combase!CoCreateInstance. That function's entry jumped through
MinHook's relay into the loaded FilePilot adapter. All observed adapter TLS
reentrancy counters in the browser were zero. This rules out a missing adapter,
an unpatched import target, or a globally enabled bypass counter at that snapshot;
it does not establish what happened during the user's earlier request.

Chromium's [download picker source](https://github.com/chromium/chromium/blob/main/chrome/browser/download/download_file_picker.cc)
requests SELECT_SAVEAS_FILE. Its [Windows dialog implementation](https://github.com/chromium/chromium/blob/main/ui/shell_dialogs/execute_select_file_win.cc)
activates FileSaveDialog through CoCreateInstance and then calls Show. These primary
sources agree with the installed binary's activation path.

## Changes made

The adapter now records successful creation before Show, failed activation,
unsupported interface requests (IID Data1), and aggregated/remote activation.
Native configuration fallback identifies its category, rather than reporting a
single generic customization reason. No filenames or selected paths are logged.

Attachment sweeps prioritize the foreground process, the most recent desktop
hook notification, and visible applications ahead of background processes. This
reduces an initial attachment delay but does not eliminate the first-dialog race.

Hosts and bootstrap DLLs are packaged in the immutable runtime directory so a
package upgrade can publish new copies while old bootstrap DLLs remain loaded.
The resident agent is restarted for deployment. Existing browser adapters are
retained; a normal browser restart loads the updated adapter without a launcher.

## Uncertainty at that earlier snapshot

No recorded request proves which fallback occurred. A dialog opened before
attachment is one possible explanation; it is not a confirmed diagnosis. Existing
native dialog objects and custom/brokered pickers remain outside creation-hook
coverage. The new diagnostics apply after the application's normal restart and
record its subsequent ordinary use. Compilation does not verify dialog behavior.
