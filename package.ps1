#Requires -Version 7
# 配布用の zip を作る。
#   1. release プリセット（SE/AE/VR 共通、開発用の機能なし）で DLL をビルドする（build-release\）
#   2. espgen で MiniSkirt.esp と SEQ\MiniSkirt.seq を作る
#   3. assets\ と DLL を dist\stage\ に集め、dist\MiniSkirt-<版>.zip にする
# 使い方: pwsh -File package.ps1 [-Clean] [-Draft]
#   -Clean  build-release\ を消して一からビルドする
#   -Draft  ソースの URL が未記入でも作る（試し用。名前が -draft になる。Nexus には上げない）
param([switch]$Clean, [switch]$Draft)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# 版は CMakeLists.txt の project(MiniSkirt VERSION x.y.z) から取る
$m = Select-String -Path CMakeLists.txt -Pattern 'project\(MiniSkirt VERSION ([0-9.]+)'
if (-not $m) { throw 'CMakeLists.txt から版が読めません' }
$version = $m.Matches[0].Groups[1].Value
$vcpkgVer = (Get-Content vcpkg.json -Raw | ConvertFrom-Json).'version-semver'
if ($vcpkgVer -ne $version) { throw "版が食い違っています: CMakeLists.txt $version / vcpkg.json $vcpkgVer" }

# ライセンス表示にソースの URL が入っているか
$notices = 'assets\SKSE\Plugins\MiniSkirt_Licenses\THIRD_PARTY_NOTICES.txt'
if ((Get-Content $notices -Raw).Contains('(source URL to be filled in before release)')) {
    if (-not $Draft) { throw "$notices にソースの URL がありません。tools\gen_notices.py で入れてください（試しなら -Draft）" }
    Write-Warning 'ソースの URL が未記入（-Draft）'
}

# 1. ビルド
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
# Load local.ps1 after Enter-VsDevShell: the VS developer shell sets VCPKG_ROOT to its bundled vcpkg
if (Test-Path "$PSScriptRoot\local.ps1") { . "$PSScriptRoot\local.ps1" }
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT が設定されていません（README.md）' }
Set-Location $PSScriptRoot
if ($Clean) { Remove-Item -Recurse -Force build-release -ErrorAction SilentlyContinue }
cmake --preset release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset release
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$dll = 'build-release\MiniSkirt.dll'
# 開発用の機能が入っていないこと（DebugCmd の文字列が無いこと）をバイト列で確かめる
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $dll))
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
if ($text.Contains('MiniSkirt_cmd')) { throw "$dll に開発用の機能（DebugCmd）が入っています" }

# 2〜3. 集める
$stage = 'dist\stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse assets\* $stage
Copy-Item $dll "$stage\SKSE\Plugins\"
dotnet run --project espgen -c Release -- (Resolve-Path $stage).Path
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# MCM の保存値（利用者の設定）は配らない
if (Test-Path "$stage\MCM\Settings") { throw 'MCM\Settings が入っています' }

$name = "MiniSkirt-$version" + ($(if ($Draft) { '-draft' } else { '' }))
$zip = "dist\$name.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
# .NET の ZipFile はフォルダの区切りを / で書く（MO2・Vortex で読める）
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory((Resolve-Path $stage).Path, (Join-Path (Resolve-Path dist).Path "$name.zip"))
# 落ちたときのログを読むための pdb は手元に残す（配らない）
Copy-Item build-release\MiniSkirt.pdb "dist\$name.pdb"

# 中身を表示し、区切りが / であることを確かめる
$z = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $zip).Path)
try {
    $bad = $z.Entries | Where-Object { $_.FullName.Contains('\') }
    if ($bad) { throw "zip の中のパスに \ があります: $($bad[0].FullName)" }
    $z.Entries | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName }
} finally { $z.Dispose() }
Write-Host "作成: $zip"
