param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\binaries\release\dialogs'),
    [string]$PickerExe = (Join-Path $PSScriptRoot '..\..\binaries\release\dialogs\FPilotPicker.exe')
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$PickerExe = [IO.Path]::GetFullPath($PickerExe)
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'Visual Studio C++ build tools were not found.' }
$dev = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
foreach ($architecture in @('x64', 'x86')) {
    $build = Join-Path $PSScriptRoot "build-$architecture"
    # Production targets only. Do not build or run conformance/test targets.
    $targets = if ($architecture -eq 'x64') {
        'FPilotDialogCom FPilotDialogAgent FPilotDialogExternal FPilotPickerRuntime'
    } else { 'FPilotDialogCom' }
    $command = 'call "{0}" -arch={1} >nul && "{2}" -S "{3}" -B "{4}" -G Ninja -DCMAKE_BUILD_TYPE=Release && "{2}" --build "{4}" --target {5}' -f $dev, $architecture, $cmake, $PSScriptRoot, $build, $targets
    cmd /d /c $command
    if ($LASTEXITCODE) { throw "$architecture compilation failed." }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$runtimeName = 'ComRuntime-1-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
$runtime = Join-Path $OutputDirectory $runtimeName
New-Item -ItemType Directory -Path $runtime | Out-Null
foreach ($file in @('FPilotDialogCom64.dll', 'FPilotPickerRuntime.dll', 'FPilotDialogAgent.exe', 'FPilotDialogExternal.exe')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "build-x64\$file") -Destination (Join-Path $runtime $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x86\FPilotDialogCom32.dll') -Destination $runtime
Copy-Item -LiteralPath $PickerExe -Destination (Join-Path $runtime 'FPilotPicker.exe')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install-com.ps1') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'COM-README.md') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor\minhook\LICENSE.txt') -Destination (Join-Path $runtime 'MinHook-LICENSE.txt')
foreach ($document in @('README.md', 'AGENT-README.md', 'EXTERNAL-README.md', 'WINRT-README.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $document) -Destination $OutputDirectory
}
Set-Content -LiteralPath (Join-Path $OutputDirectory 'com-runtime.txt.new') -Value $runtimeName -Encoding ASCII
Move-Item -LiteralPath (Join-Path $OutputDirectory 'com-runtime.txt.new') -Destination (Join-Path $OutputDirectory 'com-runtime.txt') -Force
Write-Output "COM replacement package: $runtime"
