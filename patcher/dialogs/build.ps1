param(
    [string]$InputExe = (Join-Path $PSScriptRoot '..\..\binaries\input\FPilot-0.8.5.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\binaries\release\dialogs'),
    [string]$PythonExe,
    [switch]$IncludeWinRT
)
$ErrorActionPreference = 'Stop'
$invocationDirectory = (Get-Location).Path
if (-not [IO.Path]::IsPathRooted($InputExe)) {
    $InputExe = Join-Path $invocationDirectory $InputExe
}
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $invocationDirectory $OutputDirectory
}
$InputExe = [IO.Path]::GetFullPath($InputExe)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $PythonExe) {
    $pythonCandidates = @()
    $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($pythonCommand -and $pythonCommand.Source -notlike '*\WindowsApps\*') {
        $pythonCandidates += $pythonCommand.Source
    }
    $userProfile = [Environment]::GetFolderPath('UserProfile')
    if ($userProfile) {
        $pythonCandidates += Join-Path $userProfile `
            '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    }
    $PythonExe = $pythonCandidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
        Select-Object -First 1
}
if (-not $PythonExe) { throw 'Python was not found; pass -PythonExe with a Python executable path.' }
$PythonExe = (Resolve-Path -LiteralPath $PythonExe).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$pickerExe = Join-Path $OutputDirectory 'FPilotPicker.exe'
# Recreate the helper from the original version-pinned input, never from the
# composed executable. Its UI seams depend on the original 0.8.5 layout.
& $PythonExe (Join-Path $PSScriptRoot 'patch_picker.py') $InputExe $pickerExe
if ($LASTEXITCODE -ne 0) { throw "Picker creation failed with exit code $LASTEXITCODE." }
& (Join-Path $PSScriptRoot 'build-com.ps1') -OutputDirectory $OutputDirectory -PickerExe $pickerExe
if ($IncludeWinRT) {
    & (Join-Path $PSScriptRoot 'build-winrt.ps1') -OutputDirectory $OutputDirectory
}
Write-Output "Dialog package: $OutputDirectory"
Write-Output 'Registration is unchanged. Run the packaged install-com.ps1 separately to install or upgrade.'
