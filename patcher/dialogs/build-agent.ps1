param(
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '..\..\binaries\release\dialogs'),
    [string]$PickerExe=(Join-Path $PSScriptRoot '..\..\binaries\release\dialogs\FPilotPicker.exe'),
    [switch]$PackageOnly
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$PickerExe=[IO.Path]::GetFullPath($PickerExe)
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vsInstall){throw 'Visual Studio C++ build tools were not found'}
$dev=Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
$cmake=Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!$PackageOnly) { foreach($architecture in @('x64','x86')) {
    $build=Join-Path $PSScriptRoot "build-$architecture"
    $targets=if($architecture -eq 'x64'){
        'FPilotDialogs FPilotDialogAgent FPilotDialogExternal FPilotDialogHost FPilotDialogBootstrap FPilotPickerRuntime'
    }else{'FPilotDialogs FPilotDialogHost FPilotDialogBootstrap'}
    $command='call "{0}" -arch={1} >nul && "{2}" -S "{3}" -B "{4}" -G Ninja -DCMAKE_BUILD_TYPE=Release && "{2}" --build "{4}" --target {5}' -f $dev,$architecture,$cmake,$PSScriptRoot,$build,$targets
    cmd /d /c $command
    if($LASTEXITCODE){throw "$architecture compilation failed"}
}
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$runtimeName='AgentRuntime-2-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
$runtimeDirectory=Join-Path $OutputDirectory $runtimeName
New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x64\FPilotDialogs.dll') -Destination $runtimeDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x86\FPilotDialogs32.dll') -Destination $runtimeDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x64\FPilotPickerRuntime.dll') -Destination $runtimeDirectory
Copy-Item -LiteralPath $PickerExe -Destination (Join-Path $runtimeDirectory 'FPilotPicker.exe')
foreach($artifact in @(
    @('build-x64','FPilotDialogHost64.exe'),
    @('build-x64','FPilotDialogBootstrap64.dll'),
    @('build-x86','FPilotDialogHost32.exe'),
    @('build-x86','FPilotDialogBootstrap32.dll')
)) {
    $source=Join-Path (Join-Path $PSScriptRoot $artifact[0]) $artifact[1]
    Copy-Item -LiteralPath $source -Destination (Join-Path $runtimeDirectory $artifact[1])
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x64\FPilotDialogAgent.exe') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x64\FPilotDialogExternal.exe') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-x64\FPilotDialogExternal.exe') -Destination $runtimeDirectory
# Publish the selected immutable runtime after its payload and hosts are copied.
$manifest=Join-Path $OutputDirectory 'agent-runtime.txt'
$temporaryManifest=Join-Path $OutputDirectory 'agent-runtime.txt.new'
Set-Content -LiteralPath $temporaryManifest -Value $runtimeName -Encoding ASCII
Move-Item -LiteralPath $temporaryManifest -Destination $manifest -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'AGENT-README.md') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'EXTERNAL-README.md') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor\minhook\LICENSE.txt') -Destination (Join-Path $OutputDirectory 'MinHook-LICENSE.txt')
Write-Output "Background agent package: $OutputDirectory"
