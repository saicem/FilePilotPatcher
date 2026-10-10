param(
    [string]$InputExe = (Join-Path $PSScriptRoot '..\binaries\input\FPilot-0.8.5.exe'),
    [string]$OutputExe = (Join-Path $PSScriptRoot '..\binaries\release\FPilot-0.8.5-open-location-tab-merge.exe'),
    [string]$PythonExe,
    [string]$UnicodeReport,
    [switch]$OpenLocationOnly,
    [switch]$All,
    [switch]$Archives,
    [switch]$Startup,
    [switch]$Undo,
    [switch]$Dialogs,
    [switch]$DialogsOnly,
    [switch]$WinRT,
    [string]$DialogOutputDirectory = (Join-Path $PSScriptRoot '..\binaries\release\dialogs')
)

$ErrorActionPreference = 'Stop'
$includeUnicode = $All
$includeUndo = $All -or $Undo
$includeArchives = $All -or $Archives
$includeStartup = $All -or $Startup
$includeDialogs = $All -or $Dialogs -or $DialogsOnly -or $WinRT
if ($DialogsOnly -and ($All -or $OpenLocationOnly -or $Archives -or $Startup -or $Undo)) {
    throw '-DialogsOnly cannot be combined with executable patch profiles'
}
if ($DialogsOnly) {
    & (Join-Path $PSScriptRoot 'dialogs\build.ps1') -InputExe $InputExe `
        -OutputDirectory $DialogOutputDirectory -PythonExe $PythonExe -IncludeWinRT:$WinRT
    return
}
if ($All -and $OpenLocationOnly) {
    throw '-All cannot be combined with -OpenLocationOnly'
}
if ($All -and -not $PSBoundParameters.ContainsKey('OutputExe')) {
    $OutputExe = Join-Path $PSScriptRoot '..\binaries\release\FPilot-0.8.5-all-patches.exe'
}
if ($Archives -and -not $PSBoundParameters.ContainsKey('OutputExe')) {
    $OutputExe = Join-Path $PSScriptRoot '..\binaries\release\FPilot-0.8.5-archives.exe'
}
if ($Startup -and -not $PSBoundParameters.ContainsKey('OutputExe')) {
    $OutputExe = Join-Path $PSScriptRoot '..\binaries\release\FPilot-0.8.5-optimized.exe'
}
if ($Undo -and -not $All -and -not $Archives -and -not $Startup -and -not $PSBoundParameters.ContainsKey('OutputExe')) {
    $OutputExe = Join-Path $PSScriptRoot '..\binaries\release\FPilot-0.8.5-undo.exe'
}
$invocationDirectory = (Get-Location).Path
if (-not [System.IO.Path]::IsPathRooted($InputExe)) {
    $InputExe = Join-Path $invocationDirectory $InputExe
}
if (-not [System.IO.Path]::IsPathRooted($OutputExe)) {
    $OutputExe = Join-Path $invocationDirectory $OutputExe
}
if ($includeUnicode -and -not $UnicodeReport) { $UnicodeReport = $OutputExe + '.unicode.json' }
if ($UnicodeReport -and -not [System.IO.Path]::IsPathRooted($UnicodeReport)) {
    $UnicodeReport = Join-Path $invocationDirectory $UnicodeReport
}
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsDev = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsInstall) {
        $candidate = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $candidate) { $vsDev = $candidate }
    }
}
if (-not $vsDev) {
    $candidate = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $candidate) { $vsDev = $candidate }
}
if (-not $vsDev -or -not (Test-Path -LiteralPath $vsDev)) {
    throw 'A Visual Studio x64 C++ build environment was not found'
}

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
if (-not $PythonExe -or -not (Test-Path -LiteralPath $PythonExe)) {
    throw 'Python was not found; pass -PythonExe with a Python executable path'
}
$PythonExe = (Resolve-Path -LiteralPath $PythonExe).Path

& $PythonExe -c 'import lief, capstone' 2>$null
if ($LASTEXITCODE -ne 0) {
    throw 'Python packages lief and capstone are required: python -m pip install lief capstone'
}

$compile = 'call "{0}" >nul && cl /nologo /c /O2 /GS- /GR- /EHs-c- /Zl /W4 /DUNICODE /D_UNICODE /Brepro payload.cpp /Fopayload.obj && link /nologo /Brepro /dll /nodefaultlib /entry:DllMain /base:0x140270000 /fixed:no /dynamicbase:no /machine:x64 /out:payload.dll payload.obj' -f $vsDev
$compileUnicode = 'call "{0}" >nul && cl /nologo /c /O2 /GS- /GR- /EHs-c- /Zl /W4 /DUNICODE /D_UNICODE /Brepro unicode_payload.cpp /Founicode_payload.obj && link /nologo /Brepro /dll /nodefaultlib /entry:DllMain /base:0x1402A0000 /fixed:no /dynamicbase:no /machine:x64 /out:unicode_payload.dll unicode_payload.obj' -f $vsDev
$compileMenu = 'call "{0}" >nul && cl /nologo /c /O2 /GS- /GR- /EHs-c- /Zl /W4 /DUNICODE /D_UNICODE /Brepro menu_payload.cpp /Fomenu_payload.obj && link /nologo /Brepro /dll /nodefaultlib /entry:DllMain /base:0x1402D0000 /fixed:no /dynamicbase:no /machine:x64 /out:menu_payload.dll menu_payload.obj' -f $vsDev
$compileStartup = 'call "{0}" >nul && cl /nologo /c /O2 /GS- /GR- /EHs-c- /Zl /W4 /DUNICODE /D_UNICODE /Brepro startup_payload.cpp /Fostartup_payload.obj && link /nologo /Brepro /dll /nodefaultlib /entry:DllMain /base:0x140300000 /fixed:no /dynamicbase:no /machine:x64 /out:startup_payload.dll startup_payload.obj' -f $vsDev
Push-Location $PSScriptRoot
try {
    cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) { throw "Payload build failed with exit code $LASTEXITCODE" }
    if ($includeUnicode) {
        cmd.exe /d /c $compileUnicode
        if ($LASTEXITCODE -ne 0) { throw "Unicode payload build failed with exit code $LASTEXITCODE" }
        cmd.exe /d /c $compileMenu
        if ($LASTEXITCODE -ne 0) { throw "Menu payload build failed with exit code $LASTEXITCODE" }
    }

    if ($includeArchives) {
        & (Join-Path $PSScriptRoot 'archive\build.ps1') -VsDev $vsDev
    }
    if ($includeUndo) {
        & (Join-Path $PSScriptRoot 'undo\build.ps1') -PayloadOnly
    }
    if ($includeStartup) {
        cmd.exe /d /c $compileStartup
        if ($LASTEXITCODE -ne 0) { throw "Startup payload build failed with exit code $LASTEXITCODE" }
    }
    $patchArguments = @('.\patch_filepilot.py', $InputExe, '.\payload.dll', $OutputExe)
    if ($OpenLocationOnly) { $patchArguments = @('.\patch_filepilot.py', '--open-location-only', $InputExe, '.\payload.dll', $OutputExe) }
    if ($includeUnicode) {
        $patchArguments = @($patchArguments[0], '--all', '--unicode-payload',
            '.\unicode_payload.dll', '--menu-payload', '.\menu_payload.dll',
            '--layout-json', $UnicodeReport) +
            $patchArguments[1..($patchArguments.Length - 1)]
    }
    if ($includeArchives) { $patchArguments = @($patchArguments[0], '--archives') + $patchArguments[1..($patchArguments.Length - 1)] }
    if ($includeUndo) { $patchArguments = @($patchArguments[0], '--undo') + $patchArguments[1..($patchArguments.Length - 1)] }
    if ($includeStartup) { $patchArguments = @($patchArguments[0], '--startup', '--startup-payload', '.\startup_payload.dll') + $patchArguments[1..($patchArguments.Length - 1)] }
    # Windows PowerShell 5 treats redirected native stderr as ErrorRecords.
    # LIEF diagnostics must not terminate a successful native build mid-write.
    $savedErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $PythonExe @patchArguments
        $patchExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($patchExitCode -ne 0) { throw "Patch failed with exit code $patchExitCode" }
}
finally {
    Pop-Location
}
if ($includeDialogs) {
    & (Join-Path $PSScriptRoot 'dialogs\build.ps1') -InputExe $InputExe `
        -OutputDirectory $DialogOutputDirectory -PythonExe $PythonExe -IncludeWinRT:$WinRT
}
