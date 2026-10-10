# Automatic FilePilot Open / Save As dialogs

For normal COM loading instead of this agent's remote injection, see
[COM-README.md](COM-README.md). The modes are mutually exclusive: uninstall COM
replacement before enabling this agent. Disabling COM dialogs alone retains its
registration and does not enable injection.

This is the API interception mode: supported Open/Save requests show FilePilot
directly, before native dialog display. Modern dialog wrappers call FilePilot
instead of the native `Show`; legacy W/A adapters return the selection without
opening the native picker. An adapter executes inside eligible applications,
while the FilePilot picker runs separately. Application launchers are unnecessary.

For interception without application DLL injection, the optional
[external watcher](EXTERNAL-README.md) remains available. It handles recognizable
native windows after creation and cannot guarantee suppression of their initial
display. Do not run it alongside this API agent.

The agent attaches the dialog adapter to ordinary desktop applications already
running and applications opened normally afterward. Application shortcuts need
no changes. The FilePilot picker retains its existing native UI.

Install for the current user and enable replacement immediately:

```powershell
& .\FPilotDialogAgent.exe --install
```

This registers the agent under the current user's Windows Run key for sign-in
and starts its tray process. Installation gracefully stops this package's running
external watcher before switching; hidden native dialogs are restored through
the watcher's normal shutdown. A different package's watcher is not stopped.
To use it only for the current session, run
`FPilotDialogAgent.exe --background`. Running without arguments opens its
application status window. Double-click its tray icon to reopen that window;
right-click for enable/pause, sign-in startup, exclusions, and Exit.

```powershell
& .\FPilotDialogAgent.exe --pause
& .\FPilotDialogAgent.exe --enable
& .\FPilotDialogAgent.exe --status
& .\FPilotDialogAgent.exe --quit
& .\FPilotDialogAgent.exe --uninstall
```

Uninstall removes this package's sign-in registration and stops its agent. It
does not delete binaries referenced by running applications. An Exit or pause
allows active FilePilot dialogs to finish; subsequent requests use native dialogs.
If the agent crashes or stops responding, adapters stop replacing new requests
after its heartbeat expires (15 seconds).

Exclusions are UTF-8, one executable name or full path wildcard pattern per line,
under `%LOCALAPPDATA%\Voidstar\FilePilotDialogs\exclusions.txt`. Lines beginning
with `#` are comments. The tray menu opens that file; edits apply automatically.
Already attached adapters also honor exclusions at the next request.

Keep the agent, `agent-runtime.txt`, and the selected `AgentRuntime-2-*` directory
together. Both hosts and bootstrap DLLs now live inside that runtime directory,
alongside the adapters and picker. Runtime directories are versioned;
retain older directories while an application may still use their DLLs/picker.
Close the agent and wait for its hosts to exit before replacing host binaries.
Compile/package additions using `build-agent.ps1`; it builds only production
targets and neither writes nor runs tests or validations.

The hosts cover matching x64/x86 GUI applications belonging to the current user
and session. The current adapter covers COM activation through ole32/combase,
including CoCreateInstanceEx for its supported interface family, COM factories,
and the legacy Open/Save W/A APIs. Its installation handshake reports whether
those hooks installed; an attached status does not mean every dialog feature is
supported. The status window shows permission, mitigation, and attachment failures.
Its last-request column reports FilePilot routing, native fallback categories,
unsupported option bits, or request errors without recording selected filenames.
It also records dialog/factory creation before Show, unsupported activation
interfaces (the first DWORD of the IID), and specific configuration fallback
reasons such as virtual folders, custom controls, or Save metadata. "No intercepted
request recorded" does not establish why a previously observed dialog was native.
Requests that bypass these interception points cannot be recorded in that column.
Each host displays at most 256 process records.

Discovery combines desktop message hooks with a two-second reconciliation sweep.
The foreground application, most recent discovery notification, and visible
application windows are prioritized ahead of background processes in each sweep.
It is asynchronous: a new process's first dialog can precede attachment. Existing
native dialog objects/factories and dialogs already open are not converted. Chrome
does not need a new launcher-created instance; its existing browser process is a
target for attachment. Older adapters loaded by the optional launcher are not
stacked with this one; the application status reports a required restart.
Brave/Chrome can show pickers from newly launched `chrome.mojom.UtilWin` utility
processes. A live investigation confirmed that such a child created its native
Save dialog before asynchronous attachment. The adapter now adds early parent
delivery for that child: it intercepts CreateProcessW and CreateProcessAsUserW
(including distinct kernelbase exports), temporarily creates the child suspended,
initializes its Windows loader on a secondary thread, loads the adapter, completes
the installation handshake, and then removes the suspension it added. It retains
CREATE_SUSPENDED if the caller supplied it. It preserves the native creation return
value and last-error result and forwards all other launch arguments unchanged.

Early delivery is limited to brave.exe, chrome.exe, chromium.exe, and msedge.exe
launching their own executable with exactly one each of `--type=utility`,
`--utility-sub-type=chrome.mojom.UtilWin`, and `--service-sandbox-type=none`.
Pause/exclusions apply in the parent. The child's real executable, user, session,
architecture, integrity and image/dynamic-code restrictions are checked before
loading. Debug/protected launches and AppContainer/low-integrity children are
excluded. Neither browser switches nor Windows mitigation policies are changed.

The status window's "Last early child delivery" column records the parent/child
association, installation or native fallback, stage/error, and any resume failure.
It records the latest eligible child per parent even after the child exits, without
retaining filenames or full command lines. Attachment is serialized between the
parent and discovery host for the same PID/creation time. Each remote initialization
wait and competing attachment lock wait is bounded to five seconds; failures release
the adapter-added suspension. Memory still used by a timed-out remote thread is
retained. The adapter never terminates the child. A failed release is retried via
NtResumeThread and reported if Windows still refuses it.

Restart an already attached browser once after upgrading so its parent process loads
this delivery component. A dialog already open remains native. Other creation APIs,
cross-architecture children, other brokers, and uninstrumented parents still use
asynchronous discovery; early coverage is not universal.
After a package upgrade, restart an already attached application normally to load
the new adapter. The agent never unloads or stacks adapters in a running application
and never closes your browser automatically. Old payloads remain available to
applications still using them.

Native fallback remains for unsupported customization, properties, options,
virtual Shell items, and legacy hooks/templates. Application-drawn, brokered,
AppContainer, and protected pickers are not guaranteed coverage. Restricted
third-party code loading/dynamic-code policies are honored. Low-integrity sandbox
processes and critical system processes are excluded. Elevated applications may
report access denied; they require an agent running at suitable integrity. There
is no automatic UAC elevation or protection bypass. ARM64 is not included.

No Windows DLLs or dialog COM registrations are replaced. No runtime trials,
tests, or validations were performed during implementation; compilation is the
only executed build check.
