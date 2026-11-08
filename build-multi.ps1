# Build the SE/AE/VR DLL (development build, includes dev tools) into build-multi\. Does not deploy.
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
# Load local.ps1 after Enter-VsDevShell: the VS developer shell sets VCPKG_ROOT to its bundled vcpkg
if (Test-Path "$PSScriptRoot\local.ps1") { . "$PSScriptRoot\local.ps1" }
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set (see README.md)' }
Set-Location $PSScriptRoot
if ($Clean) { Remove-Item -Recurse -Force build-multi -ErrorAction SilentlyContinue }
cmake --preset multi
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset multi
exit $LASTEXITCODE
