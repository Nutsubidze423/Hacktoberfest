# Builds the release exes and packs them with the test scripts into dist\kvm-test-<commit>.zip,
# which can be sent to a teammate who has not set up the build environment.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_test_package.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# Build inside a VS developer environment (cl, ninja, VCPKG_ROOT).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools not found. Run tools\setup_env.ps1 first.' }
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
Set-Location $root
cmake --preset x64-release | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build --preset x64-release
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$commit = (git rev-parse --short HEAD).Trim()
$dirty = if (git status --porcelain --untracked-files=no) { '-dirty' } else { '' }
$name = "kvm-test-$commit$dirty"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item "build\x64-release\host_a\kvm_host.exe", "build\x64-release\client_b\kvm_client.exe", "config.json" $stage
Copy-Item "tools\test_package\*" $stage
Set-Content -Path (Join-Path $stage 'VERSION.txt') -Value "commit $commit$dirty, protocol from common/include/kvm/protocol.h, built $(Get-Date -Format 'yyyy-MM-dd HH:mm')"

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host ''
Write-Host "Package: $zip"
