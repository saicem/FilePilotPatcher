param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\binaries\release\dialogs')
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'Visual Studio C++ build tools were not found.' }
$dev = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
foreach ($architecture in @('x64', 'x86')) {
    $build = Join-Path $PSScriptRoot "build-$architecture"
    # Build the production provider only; no conformance targets are invoked.
    $command = 'call "{0}" -arch={1} >nul && "{2}" -S "{3}" -B "{4}" -G Ninja -DCMAKE_BUILD_TYPE=Release && "{2}" --build "{4}" --target FPilotDialogWinRT' -f $dev, $architecture, $cmake, $PSScriptRoot, $build
    cmd /d /c $command
    if ($LASTEXITCODE) { throw "$architecture WinRT provider compilation failed." }
}
$comRuntimeName = (Get-Content -LiteralPath (Join-Path $OutputDirectory 'com-runtime.txt') -Raw).Trim()
if ($comRuntimeName -notmatch '^ComRuntime-1-[0-9-]+$') { throw 'The COM picker payload manifest is invalid.' }
$comRuntime = Join-Path $OutputDirectory $comRuntimeName
$runtimeName = 'WinRtRuntime-1-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
$runtime = Join-Path $OutputDirectory $runtimeName
New-Item -ItemType Directory -Path $runtime | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x64\FPilotDialogWinRT64.dll') -Destination $runtime
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x86\FPilotDialogWinRT32.dll') -Destination $runtime
foreach ($file in @('FPilotPicker.exe', 'FPilotPickerRuntime.dll')) {
    Copy-Item -LiteralPath (Join-Path $comRuntime $file) -Destination $runtime
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor\minhook\LICENSE.txt') -Destination (Join-Path $runtime 'MinHook-LICENSE.txt')
foreach ($file in @('install-winrt.ps1', 'WINRT-README.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $OutputDirectory
}
Set-Content -LiteralPath (Join-Path $OutputDirectory 'winrt-runtime.txt.new') -Value $runtimeName -Encoding ASCII
Move-Item -LiteralPath (Join-Path $OutputDirectory 'winrt-runtime.txt.new') -Destination (Join-Path $OutputDirectory 'winrt-runtime.txt') -Force
Write-Output "WinRT replacement package: $runtime"
