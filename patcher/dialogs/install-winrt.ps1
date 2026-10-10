param(
    [ValidateSet('Install', 'Uninstall', 'Enable', 'Disable')]
    [string]$Action = 'Install',
    [string]$PackageDirectory = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
if (![Environment]::Is64BitProcess) { throw 'Run this installer in 64-bit PowerShell.' }
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$settingsPath = 'Software\Voidstar\FilePilotDialogs'
$journalPath = "$settingsPath\WinRtRegistration.v1"
$activationPath = 'SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId'
$classes = @('Windows.Storage.Pickers.FileOpenPicker', 'Windows.Storage.Pickers.FileSavePicker')
$machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$user = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)

# Backup/restore privilege opens the protected, existing registration without
# taking ownership or changing its ACL. Privilege state is restored on exit.
if (-not ('FPilotWinRtNative' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
public sealed class FPilotWinRtNative : IDisposable {
    [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct Privileges { public uint Count; public Luid Id; public uint Attributes; }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool LookupPrivilegeValue(string system, string name, out Luid id);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool all, ref Privileges state, uint length, out Privileges previous, out uint returned);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegCreateKeyEx(IntPtr parent, string name, uint reserved, string cls, uint options, uint access, IntPtr security, out SafeRegistryHandle handle, out uint disposition);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegQueryValueEx(IntPtr key, string name, IntPtr reserved, out uint kind, byte[] data, ref uint bytes);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegSetValueEx(IntPtr key, string name, uint reserved, uint kind, byte[] data, uint bytes);
    [DllImport("advapi32.dll")] static extern int RegFlushKey(IntPtr key);
    IntPtr token;
    Privileges backup, restore;
    bool backupSet, restoreSet;
    Privileges Enable(string name) {
        Luid id;
        if (!LookupPrivilegeValue(null, name, out id)) throw new Win32Exception(Marshal.GetLastWin32Error());
        Privileges state = new Privileges { Count=1, Id=id, Attributes=2 }, previous;
        uint returned;
        if (!AdjustTokenPrivileges(token, false, ref state, (uint)Marshal.SizeOf(typeof(Privileges)), out previous, out returned))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        int error = Marshal.GetLastWin32Error();
        if (error != 0) throw new Win32Exception(error, "An elevated administrator token with backup/restore privileges is required.");
        return previous;
    }
    public FPilotWinRtNative() {
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            backup=Enable("SeBackupPrivilege"); backupSet=true;
            restore=Enable("SeRestorePrivilege"); restoreSet=true;
        } catch { Dispose(); throw; }
    }
    public RegistryKey OpenExisting(RegistryKey parent, string name) {
        using (RegistryKey existing=parent.OpenSubKey(name, false)) {
            if (existing == null) throw new InvalidOperationException("The Windows runtime class is not registered: " + name);
        }
        SafeRegistryHandle handle; uint disposition;
        int error=RegCreateKeyEx(parent.Handle.DangerousGetHandle(), name, 0, null, 4, 0, IntPtr.Zero, out handle, out disposition);
        if (error != 0) throw new Win32Exception(error);
        if (disposition != 2) { handle.Dispose(); throw new InvalidOperationException("The Windows runtime class disappeared during installation."); }
        return RegistryKey.FromHandle(handle, parent.View);
    }
    public static byte[] ReadRaw(RegistryKey key, string name, out uint kind) {
        uint bytes=0;
        int error=RegQueryValueEx(key.Handle.DangerousGetHandle(), name, IntPtr.Zero, out kind, null, ref bytes);
        if (error != 0) throw new Win32Exception(error);
        if (bytes > 65536) throw new InvalidOperationException("The registry value is too large.");
        byte[] data=new byte[bytes];
        error=RegQueryValueEx(key.Handle.DangerousGetHandle(), name, IntPtr.Zero, out kind, data, ref bytes);
        if (error != 0) throw new Win32Exception(error);
        return data;
    }
    public static void WriteRaw(RegistryKey key, string name, uint kind, byte[] data) {
        int error=RegSetValueEx(key.Handle.DangerousGetHandle(), name, 0, kind, data, (uint)data.Length);
        if (error != 0) throw new Win32Exception(error);
        error=RegFlushKey(key.Handle.DangerousGetHandle());
        if (error != 0) throw new Win32Exception(error);
    }
    public void Dispose() {
        if (token == IntPtr.Zero) return;
        Privileges ignored; uint returned;
        if (restoreSet) AdjustTokenPrivileges(token, false, ref restore, (uint)Marshal.SizeOf(typeof(Privileges)), out ignored, out returned);
        if (backupSet) AdjustTokenPrivileges(token, false, ref backup, (uint)Marshal.SizeOf(typeof(Privileges)), out ignored, out returned);
        CloseHandle(token); token=IntPtr.Zero;
    }
}
'@
}

function Set-Enabled([bool]$enabled) {
    $settings = $user.CreateSubKey($settingsPath)
    try { $settings.SetValue('WinRtEnabled', [int]$enabled, [Microsoft.Win32.RegistryValueKind]::DWord); $settings.Flush() }
    finally { $settings.Dispose() }
}
function Restore-Entries($journal, $privileges) {
    foreach ($viewName in @('Registry64', 'Registry32')) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::$viewName)
        $parent = $base.OpenSubKey($activationPath, $false)
        try {
            foreach ($class in $classes) {
                $entry = $journal.OpenSubKey("$viewName\$class", $true)
                if (!$entry) { continue }
                $key = $null
                try {
                    if (!$entry.GetValue('Ready', 0)) { continue }
                    $key = $privileges.OpenExisting($parent, $class)
                    [uint32]$kind = 0
                    [byte[]]$current = [FPilotWinRtNative]::ReadRaw($key, 'DllPath', [ref]$kind)
                    [byte[]]$original = $entry.GetValue('OriginalData')
                    [uint32]$originalKind = $entry.GetValue('OriginalKind')
                    if ($kind -eq $originalKind -and [Convert]::ToBase64String($current) -eq [Convert]::ToBase64String($original)) { continue }
                    $path = [string]$key.GetValue('DllPath', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    if ($path -ne $entry.GetValue('InstalledPath') -and $path -ne $entry.GetValue('PendingPath')) {
                        throw "Another provider owns $viewName/$class. Recovery journal retained; its registration was not changed."
                    }
                    [FPilotWinRtNative]::WriteRaw($key, 'DllPath', $originalKind, $original)
                    $entry.SetValue('Restored', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
                    $entry.Flush()
                } finally { if ($key) { $key.Dispose() }; $entry.Dispose() }
            }
        } finally { if ($parent) { $parent.Dispose() }; $base.Dispose() }
    }
}

$journal = $null
$privileges = $null
$installationMutex = $null
$mutexOwned = $false
try {
    if ($Action -eq 'Enable' -or $Action -eq 'Disable') {
        $journal = $machine.OpenSubKey($journalPath, $false)
        if (!$journal -or $journal.GetValue('State') -ne 'Installed') { throw 'The machine WinRT provider is not installed.' }
        Set-Enabled ($Action -eq 'Enable')
        Write-Output "WinRT FilePilot replacement $($Action.ToLower())d for this user."
        return
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run install/uninstall from elevated 64-bit PowerShell.' }
    $installationMutex = [Threading.Mutex]::new($false, 'Global\FilePilotDialogs.WinRtRegistration.v1')
    try { $mutexOwned = $installationMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $mutexOwned = $true }
    if (!$mutexOwned) { throw 'Another WinRT installer is modifying the registration. Let it finish before retrying.' }
    $privileges = New-Object FPilotWinRtNative
    $journal = $machine.OpenSubKey($journalPath, $true)
    if ($journal -and ($journal.GetValue('Version') -ne 1 -or $journal.GetValue('Owner') -ne 'FilePilot WinRT provider v1')) {
        throw 'An unrecognized installer owns the WinRT recovery journal.'
    }
    if ($Action -eq 'Uninstall') {
        if (!$journal) { Write-Output 'No WinRT replacement registration is installed.'; return }
        Set-Enabled $false
        Restore-Entries $journal $privileges
        $journal.Dispose(); $journal = $null
        $machine.DeleteSubKeyTree($journalPath)
        Write-Output 'Original Windows WinRT Open/Save registrations restored. Payloads retained for applications still using them.'
        return
    }
    if ($journal -and $journal.GetValue('State') -ne 'Installed') {
        throw 'An interrupted WinRT installation has a recovery journal. Run -Action Uninstall before installing again.'
    }
    $runtimeName = (Get-Content -LiteralPath (Join-Path $PackageDirectory 'winrt-runtime.txt') -Raw).Trim()
    if ($runtimeName -notmatch '^WinRtRuntime-1-[0-9-]+$') { throw 'The WinRT payload manifest is invalid.' }
    $source = Join-Path $PackageDirectory $runtimeName
    # Machine activation must not depend on a DLL in a user-writable directory.
    $installRoot = Join-Path $env:ProgramFiles 'Voidstar\FilePilotDialogs'
    $runtime = Join-Path $installRoot $runtimeName
    New-Item -ItemType Directory -Force -Path $installRoot | Out-Null
    New-Item -ItemType Directory -Path $runtime | Out-Null
    foreach ($file in @('FPilotDialogWinRT64.dll', 'FPilotDialogWinRT32.dll', 'FPilotPicker.exe', 'FPilotPickerRuntime.dll')) {
        Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $runtime $file)
    }
    Copy-Item -LiteralPath (Join-Path $PackageDirectory 'install-winrt.ps1') -Destination (Join-Path $installRoot 'install-winrt.ps1') -Force
    Copy-Item -LiteralPath (Join-Path $PackageDirectory 'WINRT-README.md') -Destination (Join-Path $installRoot 'WINRT-README.md') -Force
    $newInstall = !$journal
    if ($newInstall) {
        $journal = $machine.CreateSubKey($journalPath)
        $journal.SetValue('Version', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
        $journal.SetValue('Owner', 'FilePilot WinRT provider v1')
    } elseif ($journal.GetValue('Owner') -ne 'FilePilot WinRT provider v1') { throw 'A different installer owns the WinRT recovery journal.' }
    $journal.SetValue('State', 'Applying')
    $journal.SetValue('RuntimeDirectory', $runtime)
    $journal.Flush()
    try {
        foreach ($viewName in @('Registry64', 'Registry32')) {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::$viewName)
            $parent = $base.OpenSubKey($activationPath, $false)
            $dllName = if ($viewName -eq 'Registry64') { 'FPilotDialogWinRT64.dll' } else { 'FPilotDialogWinRT32.dll' }
            $system = if ($viewName -eq 'Registry64') { 'System32' } else { 'SysWOW64' }
            $nativePath = Join-Path $env:windir "$system\twinui.appcore.dll"
            try {
                foreach ($class in $classes) {
                    $key = $null; $entry = $null
                    try {
                        $key = $privileges.OpenExisting($parent, $class)
                        $current = [string]$key.GetValue('DllPath', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                        $entry = $journal.OpenSubKey("$viewName\$class", $true)
                        if (!$entry) {
                            if ([Environment]::ExpandEnvironmentVariables($current) -ne $nativePath -or $key.GetValue('ActivationType') -ne 0) {
                                throw "Unsupported original WinRT provider for $viewName/${class}: $current"
                            }
                            [uint32]$kind = 0
                            [byte[]]$data = [FPilotWinRtNative]::ReadRaw($key, 'DllPath', [ref]$kind)
                            $entry = $journal.CreateSubKey("$viewName\$class")
                            $entry.SetValue('OriginalKind', [int]$kind, [Microsoft.Win32.RegistryValueKind]::DWord)
                            $entry.SetValue('OriginalData', $data, [Microsoft.Win32.RegistryValueKind]::Binary)
                            $entry.SetValue('Ready', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
                        } elseif ($current -ne $entry.GetValue('InstalledPath')) { throw "Another provider owns $viewName/$class." }
                        $path = Join-Path $runtime $dllName
                        $entry.SetValue('PendingPath', $path)
                        $entry.Flush(); $journal.Flush()
                        [byte[]]$data = [Text.Encoding]::Unicode.GetBytes($path + [char]0)
                        [FPilotWinRtNative]::WriteRaw($key, 'DllPath', 1, $data)
                        $entry.SetValue('InstalledPath', $path)
                        $entry.Flush()
                    } finally { if ($key) { $key.Dispose() }; if ($entry) { $entry.Dispose() } }
                }
            } finally { if ($parent) { $parent.Dispose() }; $base.Dispose() }
        }
        $journal.SetValue('State', 'Installed'); $journal.Flush()
        if ($newInstall) { Set-Enabled $true }
    } catch {
        $installError = $_
        Set-Enabled $false
        try {
            Restore-Entries $journal $privileges
            $journal.Dispose(); $journal = $null
            $machine.DeleteSubKeyTree($journalPath)
        } catch { Write-Warning "Recovery is incomplete; retain $journalPath and run -Action Uninstall. $_" }
        throw $installError
    }
    Write-Output "FilePilot WinRT Open/Save provider installed (x64/x86): $runtime"
    Write-Output 'Fully close and reopen applications to release cached WinRT factories. No applications were terminated.'
    Write-Output 'Existing COM replacement and external fallback remain installed. Sandboxed/unsupported WinRT requests delegate to Windows.'
} finally {
    if ($journal) { $journal.Dispose() }
    if ($privileges) { $privileges.Dispose() }
    if ($mutexOwned) { $installationMutex.ReleaseMutex() }
    if ($installationMutex) { $installationMutex.Dispose() }
    $machine.Dispose(); $user.Dispose()
}
