# One-time developer machine setup for the KVM demo (Windows 10/11 x64).
# Installs Visual Studio 2022 Build Tools (compiler only, no IDE) with MSVC, CMake, Ninja, vcpkg and the
# Windows 11 SDK. Run from a normal PowerShell; the installer asks for admin rights (UAC).
#
#   powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1
#   powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1 -Firewall   # also open ports (run as admin)

param(
    [switch]$Firewall
)

$ErrorActionPreference = 'Stop'

$components = @(
    'Microsoft.VisualStudio.Workload.VCTools',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.VC.CMake.Project',
    'Microsoft.VisualStudio.Component.Vcpkg',
    'Microsoft.VisualStudio.Component.Windows11SDK.22621'
)

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

if ($vsPath) {
    Write-Host "Visual Studio with C++ tools found: $vsPath"
} else {
    Write-Host 'Installing Visual Studio 2022 Build Tools + C++ (~3 GB, 10-30 min)...'
    $override = '--wait --passive --norestart ' + (($components | ForEach-Object { "--add $_" }) -join ' ')
    winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget `
        --accept-package-agreements --accept-source-agreements --override $override
    if ($LASTEXITCODE -ne 0) { throw "winget failed with exit code $LASTEXITCODE" }
}

if ($Firewall) {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Firewall rules need an elevated PowerShell (Run as administrator).'
    }
    if (-not (Get-NetFirewallRule -DisplayName 'KVM demo (TCP 50500-50502)' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName 'KVM demo (TCP 50500-50502)' -Direction Inbound -Protocol TCP `
            -LocalPort 50500-50502 -RemoteAddress LocalSubnet -Profile Any -Action Allow | Out-Null
    }
    Write-Host 'Firewall: TCP 50500-50502 open for the local network (any profile).'
}

Write-Host ''
Write-Host 'Next: open "Developer PowerShell for VS 2022" in the repo folder and run:'
Write-Host '  cmake --preset x64-release'
Write-Host '  cmake --build --preset x64-release'
