# Builds Sarke's installer: dist\SarkeSetup-<version>.exe, one file that sets up a PC completely
# (see installer\sarke.iss for what goes in). The version comes from resources\version.h.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
#
# Needs the build environment (tools\setup_env.ps1) and Inno Setup 6
# (winget install JRSoftware.InnoSetup). A running Sarke from this repo is asked to quit first,
# since the build replaces its executables.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found: winget install JRSoftware.InnoSetup' }

# The build replaces the executables a running Sarke holds.
$sarke = Join-Path $root 'build\x64-release\tools\sarke.exe'
if ((Test-Path $sarke) -and (Get-Process sarke, kvm_host, kvm_client -ErrorAction SilentlyContinue)) {
    Write-Host 'Asking the running Sarke to quit...'
    & $sarke --quit
    Start-Sleep -Seconds 4
}

# Build inside a VS developer environment (cl, rc, ninja, VCPKG_ROOT).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) { $vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' 2>$null | Out-Null
Set-Location $root
cmake --preset x64-release | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build --preset x64-release
if ($LASTEXITCODE -ne 0) { throw 'build failed (is Sarke still running?)' }

# The quick automatic tests must pass before anything ships.
foreach ($test in 'protocol_test', 'queue_test', 'file_transfer_test', 'clipboard_test', 'kvm_link_test', 'launcher_test', 'display_layout_test') {
    # The tests log to stderr; Windows PowerShell would turn that into errors under 'Stop'.
    $p = Start-Process -FilePath "build\x64-release\tools\$test.exe" -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput "$env:TEMP\sarke_$test.out" -RedirectStandardError "$env:TEMP\sarke_$test.err"
    if ($p.ExitCode -ne 0) { throw "$test failed - not building the installer (see $env:TEMP\sarke_$test.err)" }
}
Write-Host 'Tests passed.'

# The driver package isn't in git: fetch the pinned, hash-checked copy (skipped when present).
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fetch_vdd.ps1')
if ($LASTEXITCODE -ne 0) { throw 'fetch_vdd.ps1 failed - not building the installer' }

& $iscc /Q "installer\sarke.iss"
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
$setup = Get-ChildItem (Join-Path $root 'dist') -Filter 'SarkeSetup-*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host ''
Write-Host "Installer: $($setup.FullName) ($([math]::Round($setup.Length / 1MB, 1)) MB)"
