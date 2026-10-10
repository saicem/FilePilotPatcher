param(
    [string]$Action = 'Install',
    [string]$PackageDirectory = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$settingsPath = 'Software\Voidstar\FilePilotDialogs'
$journalPath = "$settingsPath\ComRegistration.v1"
$runPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
$fallbackStartupName = 'FilePilot Dialog Fallback'
$classes = @('{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}', '{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}')

# Raw registry tree copies preserve unknown value types, bytes and subkeys.
# The journal is flushed before any COM registration is modified.
if (-not ('FPilotComNative' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Diagnostics;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class FPilotComNative {
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    public static extern int RegCopyTree(IntPtr source, string subkey, IntPtr destination);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    public static extern int RegQueryValueEx(IntPtr key, string name, IntPtr reserved,
        out uint type, byte[] data, ref uint bytes);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    public static extern int RegSetValueEx(IntPtr key, string name, uint reserved,
        uint type, byte[] data, uint bytes);
    public delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    public static extern bool EnumThreadWindows(uint thread, WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder className, int size);
    public static IntPtr ControllerWindow(uint pid, string className) {
        IntPtr found = IntPtr.Zero;
        WindowCallback callback = delegate(IntPtr window, IntPtr parameter) {
            StringBuilder name = new StringBuilder(256);
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == pid && GetClassName(window, name, name.Capacity) != 0 &&
                String.Equals(name.ToString(), className, StringComparison.Ordinal)) {
                found = window; return false;
            }
            return true;
        };
        using (Process process = Process.GetProcessById((int)pid)) {
            foreach (ProcessThread thread in process.Threads) {
                EnumThreadWindows((uint)thread.Id, callback, IntPtr.Zero);
                if (found != IntPtr.Zero) break;
            }
        }
        return found;
    }
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder image, ref uint length);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@
}

function Copy-RegistryTree($source, $destination) {
    $errorCode = [FPilotComNative]::RegCopyTree($source.Handle.DangerousGetHandle(), $null, $destination.Handle.DangerousGetHandle())
    if ($errorCode) { throw "Registry backup/restore failed (Windows error $errorCode)." }
    $destination.Flush()
}
function Copy-RegistryValue($source, $destination, [string]$name) {
    [uint32]$kind = 0; [uint32]$bytes = 0
    $errorCode = [FPilotComNative]::RegQueryValueEx($source.Handle.DangerousGetHandle(), $name, [IntPtr]::Zero, [ref]$kind, $null, [ref]$bytes)
    if ($errorCode -eq 2) { return }
    if ($errorCode) { throw "Cannot read startup value (Windows error $errorCode)." }
    $data = New-Object byte[] $bytes
    $errorCode = [FPilotComNative]::RegQueryValueEx($source.Handle.DangerousGetHandle(), $name, [IntPtr]::Zero, [ref]$kind, $data, [ref]$bytes)
    if ($errorCode) { throw "Cannot read startup value (Windows error $errorCode)." }
    $errorCode = [FPilotComNative]::RegSetValueEx($destination.Handle.DangerousGetHandle(), $name, 0, $kind, $data, $bytes)
    if ($errorCode) { throw "Cannot restore startup value (Windows error $errorCode)." }
    $destination.Flush()
}
function Set-Dword($key, [string]$name, [int]$value) {
    $key.SetValue($name, $value, [Microsoft.Win32.RegistryValueKind]::DWord)
}
function Same-Path([string]$left, [string]$right) {
    return [string]::Equals($left, $right, [StringComparison]::OrdinalIgnoreCase)
}
function Stop-Controller([string]$className, [string]$executable, [uint32]$message, [uint32]$command) {
    $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    foreach ($controller in [Diagnostics.Process]::GetProcessesByName([IO.Path]::GetFileNameWithoutExtension($executable))) {
    try {
        if ($controller.SessionId -ne $session) { continue }
        [uint32]$controllerPid = $controller.Id
        $processHandle = [FPilotComNative]::OpenProcess(0x101000, $false, $controllerPid)
        if ($processHandle -eq [IntPtr]::Zero) {
            if ($controller.HasExited) { continue }
            throw "Cannot stop $executable safely. Close its FilePilot controller and retry."
        }
        try {
        $image = New-Object Text.StringBuilder 32768; [uint32]$length = 32768
        if (![FPilotComNative]::QueryFullProcessImageName($processHandle, 0, $image, [ref]$length)) {
            throw "Cannot identify the running $executable controller."
        }
        $path = $image.ToString(); $parent = [IO.Path]::GetDirectoryName($path)
        $owned = (Same-Path $path (Join-Path $PackageDirectory $executable)) -or (
            (Same-Path ([IO.Path]::GetDirectoryName($parent)) $PackageDirectory) -and
            ([IO.Path]::GetFileName($parent).StartsWith('AgentRuntime-2-', [StringComparison]::Ordinal) -or
             [IO.Path]::GetFileName($parent).StartsWith('ComRuntime-1-', [StringComparison]::Ordinal)) -and
            (Same-Path ([IO.Path]::GetFileName($path)) $executable))
        if (!$owned) { throw "A controller from another FilePilot package is running: $path. Close that controller before installing." }
        if ([FPilotComNative]::WaitForSingleObject($processHandle, 0) -eq 0) { continue }
        $window = [FPilotComNative]::ControllerWindow($controllerPid, $className)
        if ($window -eq [IntPtr]::Zero) {
            if ([FPilotComNative]::WaitForSingleObject($processHandle, 0) -eq 0) { continue }
            # Immutable external executables run isolated --dialog workers.
            # Their owning root controller performs normal cancellation.
            if ($className -eq 'FPilotDialogExternal.v1' -and
                !(Same-Path $parent $PackageDirectory)) { continue }
            throw "Cannot locate the $executable control window. Close its FilePilot controller and retry."
        }
        [uint32]$currentPid = 0
        $null = [FPilotComNative]::GetWindowThreadProcessId($window, [ref]$currentPid)
        if ($currentPid -ne $controllerPid) { throw 'The controller changed during shutdown.' }
        if (![FPilotComNative]::PostMessage($window, $message, [UIntPtr]$command, [IntPtr]::Zero)) {
            throw "Cannot request normal shutdown of $executable."
        }
        if ([FPilotComNative]::WaitForSingleObject($processHandle, 15000) -ne 0) {
            throw "The $executable controller did not exit. Close it and retry. Applications have not been terminated."
        }
        } finally { $null = [FPilotComNative]::CloseHandle($processHandle) }
    } finally { $controller.Dispose() }
    }
}

$views = @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)
$main = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $views[0])
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$mutex = New-Object Threading.Mutex($false, "Global\FPilotDialogs.ComRegistration.$sid")
$locked = $false; $settings = $null; $journal = $null
try {
    try { $locked = $mutex.WaitOne(15000) }
    catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Another FilePilot COM installation is in progress.' }
    $journal = $main.OpenSubKey($journalPath, $true)
    $settings = $main.CreateSubKey($settingsPath)

    function Restore-Registrations {
        if (!$journal) { return }
        $owner = [string]$journal.GetValue('Owner', '')
        $conflicts = New-Object 'Collections.Generic.List[string]'
        foreach ($view in $views) {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
            try {
                foreach ($clsid in $classes) {
                    $record = $journal.OpenSubKey("$view\$clsid", $true)
                    if (!$record) { continue }
                    try {
                        if (!$record.GetValue('Prepared', 0) -or $record.GetValue('Restored', 0)) { continue }
                        $classPath = "Software\Classes\CLSID\$clsid"
                        $classKey = $base.OpenSubKey($classPath, $true)
                        $server = if ($classKey) { $classKey.OpenSubKey('InprocServer32', $true) } else { $null }
                        $backup = $record.OpenSubKey('Original\InprocServer32')
                        try {
                            $currentOwner = if ($server) { [string]$server.GetValue('FilePilotOwner', '') } else { '' }
                            $currentPath = if ($server) { [string]$server.GetValue('', '') } else { '' }
                            $originalPath = if ($backup) { [string]$backup.GetValue('', '') } else { '' }
                            if ($currentOwner -eq $owner -and (
                                (Same-Path $currentPath ([string]$record.GetValue('InstalledServer', ''))) -or
                                (Same-Path $currentPath ([string]$record.GetValue('PreviousServer', ''))) -or
                                (Same-Path $currentPath $originalPath))) {
                                # Remove only the server subtree we modified. Other CLSID
                                # subkeys created since installation remain intact.
                                Set-Dword $record 'RestoreStarted' 1; $record.Flush()
                                if ($server) { $server.Dispose(); $server = $null }
                                $classKey.DeleteSubKeyTree('InprocServer32', $false)
                                if ($record.GetValue('ServerExisted', 0)) {
                                    $restoredServer = $classKey.CreateSubKey('InprocServer32')
                                    try { Copy-RegistryTree $backup $restoredServer }
                                    finally { $restoredServer.Dispose() }
                                }
                                if (!$record.GetValue('ClassExisted', 0) -and
                                    $classKey.SubKeyCount -eq 0 -and $classKey.ValueCount -eq 0) {
                                    $classKey.Dispose(); $classKey = $null
                                    $base.DeleteSubKey($classPath, $false)
                                }
                            } elseif (!$server -and $record.GetValue('RestoreStarted', 0) -and $record.GetValue('ServerExisted', 0)) {
                                if (!$classKey) { $classKey = $base.CreateSubKey($classPath) }
                                $restoredServer = $classKey.CreateSubKey('InprocServer32')
                                try { Copy-RegistryTree $backup $restoredServer }
                                finally { $restoredServer.Dispose() }
                            } elseif (!$currentOwner -and !$record.GetValue('MutationStarted', 0)) {
                                # Prepared backup, but no registration write was begun.
                            } elseif (!$server -and !$record.GetValue('ServerExisted', 0)) {
                                # Already removed while recovering an interrupted uninstall.
                            } else {
                                $conflicts.Add("$view $clsid")
                                continue
                            }
                            Set-Dword $record 'Restored' 1; $record.Flush()
                        } finally {
                            if ($backup) { $backup.Dispose() }
                            if ($server) { $server.Dispose() }
                            if ($classKey) { $classKey.Dispose() }
                        }
                    } finally { $record.Dispose() }
                }
            } finally { $base.Dispose() }
        }
        if ($conflicts.Count) {
            throw ('COM registrations changed outside this installer; their backups were retained: ' + ($conflicts -join ', '))
        }
    }
    function Restore-Startup {
        $startup = $journal.OpenSubKey('Startup')
        if (!$startup) { return }
        $run = $main.CreateSubKey($runPath)
        try {
            foreach ($name in @('FilePilot Dialog Agent')) {
                if ($startup.GetValue("Removed:$name", 0) -and !($run.GetValueNames() -contains $name)) {
                    Copy-RegistryValue $startup $run $name
                }
            }
        } finally { $run.Dispose(); $startup.Dispose() }
    }
    function Remove-FallbackStartup {
        if (!$journal) { return }
        $expected = [string]$journal.GetValue('FallbackStartupCommand', '')
        if (!$expected) { return }
        $run = $main.OpenSubKey($runPath, $true)
        if (!$run) { return }
        try {
            if (Same-Path ([string]$run.GetValue($fallbackStartupName, '')) $expected) {
                $run.DeleteValue($fallbackStartupName, $false); $run.Flush()
            }
        } finally { $run.Dispose() }
    }

    switch ($Action.ToLowerInvariant()) {
        'enable' {
            if (!$journal -or !$journal.GetValue('Complete', 0)) { throw 'Install the COM package first.' }
            Set-Dword $settings 'ComEnabled' 1; $settings.Flush()
            Write-Output 'FilePilot COM dialogs enabled.'
        }
        'disable' {
            Set-Dword $settings 'ComEnabled' 0; $settings.Flush()
            Write-Output 'FilePilot COM dialogs disabled; registered bridges delegate to Windows.'
        }
        'uninstall' {
            if (!$journal) { Write-Output 'No FilePilot COM registration backup is present.'; break }
            Set-Dword $settings 'ComEnabled' 0; $settings.Flush()
            Stop-Controller 'FPilotDialogExternal.v1' 'FPilotDialogExternal.exe' 0x8099 206
            Remove-FallbackStartup
            Restore-Registrations
            Restore-Startup
            Set-Dword $settings 'ComModeInstalled' 0; $settings.Flush()
            $journal.Dispose(); $journal = $null
            $main.DeleteSubKeyTree($journalPath, $false)
            Write-Output 'Original per-user COM registrations and removed startup entries restored. Restart applications to release cached factories.'
        }
        'install' {
            $manifest = Join-Path $PackageDirectory 'com-runtime.txt'
            $runtimeName = [IO.File]::ReadAllText($manifest).Trim()
            if ($runtimeName -notmatch '^ComRuntime-1-[A-Za-z0-9-]+$') { throw 'Invalid COM package manifest.' }
            $runtime = Join-Path $PackageDirectory $runtimeName
            foreach ($file in @('FPilotDialogCom64.dll', 'FPilotDialogCom32.dll', 'FPilotPicker.exe',
                               'FPilotPickerRuntime.dll', 'FPilotDialogAgent.exe', 'FPilotDialogExternal.exe')) {
                if (![IO.File]::Exists((Join-Path $runtime $file))) { throw "The COM package is incomplete: $file." }
            }
            if ($journal -and !$journal.GetValue('Complete', 0)) {
                throw 'An interrupted COM installation has a recovery journal. Run -Action Uninstall before reinstalling.'
            }
            if ($journal) {
                # Finish conflict detection before changing an upgrade journal.
                foreach ($view in $views) {
                    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
                    try {
                        foreach ($clsid in $classes) {
                            $record = $journal.OpenSubKey("$view\$clsid")
                            $server = $base.OpenSubKey("Software\Classes\CLSID\$clsid\InprocServer32")
                            try {
                                if (!$record -or !$server -or $server.GetValue('FilePilotOwner', '') -ne $journal.GetValue('Owner') -or
                                    !(Same-Path ([string]$server.GetValue('', '')) ([string]$record.GetValue('InstalledServer', '')))) {
                                    throw "Another program changed $view $clsid. Its registration will not be overwritten."
                                }
                            } finally { if ($record) { $record.Dispose() }; if ($server) { $server.Dispose() } }
                        }
                    } finally { $base.Dispose() }
                }
            }
            $fallbackCommand = '"' + (Join-Path $PackageDirectory 'FPilotDialogExternal.exe') + '" --background'
            $run = $main.OpenSubKey($runPath)
            try {
                $existingFallback = if ($run) { [string]$run.GetValue($fallbackStartupName, '') } else { '' }
                if ($existingFallback -and !(Same-Path $existingFallback $fallbackCommand)) {
                    throw 'Another command owns the FilePilot Dialog Fallback startup entry; it will not be overwritten.'
                }
            } finally { if ($run) { $run.Dispose() } }
            if (!$journal) {
                $journal = $main.CreateSubKey($journalPath)
                $journal.SetValue('Owner', [Guid]::NewGuid().ToString('D'))
                Set-Dword $journal 'Version' 1
                Set-Dword $journal 'Complete' 0
                $journal.Flush()
            }
            $owner = [string]$journal.GetValue('Owner')
            Set-Dword $journal 'Complete' 0; $journal.Flush()
            # Prepare every backup before publishing either architecture's override.
            foreach ($view in $views) {
                $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
                try {
                    foreach ($clsid in $classes) {
                        $record = $journal.CreateSubKey("$view\$clsid")
                        $classKey = $base.OpenSubKey("Software\Classes\CLSID\$clsid")
                        $server = if ($classKey) { $classKey.OpenSubKey('InprocServer32') } else { $null }
                        try {
                            if (!$record.GetValue('Prepared', 0)) {
                                Set-Dword $record 'ClassExisted' ([int]($null -ne $classKey))
                                Set-Dword $record 'ServerExisted' ([int]($null -ne $server))
                                if ($classKey) {
                                    $backup = $record.CreateSubKey('Original')
                                    try { Copy-RegistryTree $classKey $backup } finally { $backup.Dispose() }
                                }
                                Set-Dword $record 'Prepared' 1
                            } elseif (!$server -or $server.GetValue('FilePilotOwner', '') -ne $owner -or
                                !(Same-Path ([string]$server.GetValue('', '')) ([string]$record.GetValue('InstalledServer', '')))) {
                                throw "Another program changed $view $clsid. Its registration will not be overwritten."
                            }
                            $record.SetValue('PreviousServer', $(if ($server) { [string]$server.GetValue('', '') } else { '' }))
                            $dll = if ($view -eq $views[0]) { 'FPilotDialogCom64.dll' } else { 'FPilotDialogCom32.dll' }
                            $record.SetValue('InstalledServer', (Join-Path $runtime $dll))
                            Set-Dword $record 'Restored' 0
                            Set-Dword $record 'RestoreStarted' 0
                            $record.Flush()
                        } finally {
                            if ($server) { $server.Dispose() }
                            if ($classKey) { $classKey.Dispose() }
                            $record.Dispose()
                        }
                    }
                } finally { $base.Dispose() }
            }
            Set-Dword $journal 'Complete' 0; $journal.Flush()
            try {
                Set-Dword $settings 'ComModeInstalled' 1
                Set-Dword $settings 'ComEnabled' 0; $settings.Flush()
                Stop-Controller 'FPilotDialogAgent.v2' 'FPilotDialogAgent.exe' 0x8077 106
                Stop-Controller 'FPilotDialogExternal.v1' 'FPilotDialogExternal.exe' 0x8099 206
                Stop-Controller 'FPilotDialogHost64.v2' 'FPilotDialogHost64.exe' 0x10 0
                Stop-Controller 'FPilotDialogHost32.v2' 'FPilotDialogHost32.exe' 0x10 0

                $startup = $journal.CreateSubKey('Startup'); $run = $main.OpenSubKey($runPath, $true)
                try {
                    if ($run) {
                        foreach ($pair in @(
                            @('FilePilot Dialog Agent', 'FPilotDialogAgent.exe'),
                            @('FilePilot Dialog Agent', 'FPilotDialogExternal.exe'))) {
                            $name = $pair[0]; $expected = '"' + (Join-Path $PackageDirectory $pair[1]) + '" --background'
                            if (Same-Path ([string]$run.GetValue($name, '')) $expected) {
                                if (!($startup.GetValueNames() -contains $name)) { Copy-RegistryValue $run $startup $name }
                                Set-Dword $startup "Removed:$name" 1; $startup.Flush()
                                $run.DeleteValue($name, $false); $run.Flush()
                            }
                        }
                    }
                } finally { if ($run) { $run.Dispose() }; $startup.Dispose() }
                # Publish controller guards after their previous versions exited.
                foreach ($file in @('FPilotDialogAgent.exe', 'FPilotDialogExternal.exe')) {
                    Copy-Item -LiteralPath (Join-Path $runtime $file) -Destination (Join-Path $PackageDirectory $file) -Force
                }
                foreach ($view in $views) {
                    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
                    try {
                        foreach ($clsid in $classes) {
                            $record = $journal.OpenSubKey("$view\$clsid", $true)
                            try {
                                Set-Dword $record 'MutationStarted' 1; $record.Flush()
                                $server = $base.CreateSubKey("Software\Classes\CLSID\$clsid\InprocServer32")
                                try {
                                    $server.SetValue('FilePilotOwner', $owner)
                                    $server.SetValue('ThreadingModel', 'Apartment')
                                    $server.SetValue('', [string]$record.GetValue('InstalledServer'))
                                    $server.Flush()
                                } finally { $server.Dispose() }
                            } finally { $record.Dispose() }
                        }
                    } finally { $base.Dispose() }
                }
                Set-Dword $journal 'Complete' 1; $journal.Flush()
                Set-Dword $settings 'ComEnabled' 1; $settings.Flush()
                # Journal ownership before enabling the non-injecting fallback.
                if (!$journal.GetValue('FallbackStartupCommand', '')) {
                    Set-Dword $settings 'ExternalEnabled' 1; $settings.Flush()
                }
                $journal.SetValue('FallbackStartupCommand', $fallbackCommand); $journal.Flush()
                $run = $main.CreateSubKey($runPath)
                try { $run.SetValue($fallbackStartupName, $fallbackCommand); $run.Flush() }
                finally { $run.Dispose() }
                Start-Process -FilePath (Join-Path $PackageDirectory 'FPilotDialogExternal.exe') -ArgumentList '--background' -WindowStyle Hidden
            } catch {
                $failure = $_
                Set-Dword $settings 'ComEnabled' 0; $settings.Flush()
                try {
                    Stop-Controller 'FPilotDialogExternal.v1' 'FPilotDialogExternal.exe' 0x8099 206
                    Remove-FallbackStartup
                    Restore-Registrations
                    Restore-Startup
                    Set-Dword $settings 'ComModeInstalled' 0; $settings.Flush()
                    $journal.Dispose(); $journal = $null
                    $main.DeleteSubKeyTree($journalPath, $false)
                } catch { Write-Warning "Recovery journal retained: $($_.Exception.Message)" }
                throw $failure
            }
            Write-Output "FilePilot modern-dialog COM replacement installed for this user (x64 and x86): $runtime"
            Write-Output 'External fallback for recognizable native Open/Save dialogs starts at sign-in; its pause preference is retained on upgrades.'
            Write-Output 'Fully close and reopen applications to release old injected adapters and cached dialog factories. No application was terminated.'
        }
        default { throw 'Use -Action Install, Uninstall, Enable, or Disable.' }
    }
} finally {
    if ($journal) { $journal.Dispose() }
    if ($settings) { $settings.Dispose() }
    $main.Dispose()
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
