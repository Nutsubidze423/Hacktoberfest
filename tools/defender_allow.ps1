# Tells Microsoft Defender to trust this project's own build output.
#
# Defender's machine-learning heuristic ("Trojan:Win32/Bearfoos.B!ml") flags freshly built, unsigned
# executables that install global keyboard/mouse hooks and inject input - the same primitives a KVM
# app and a keylogger share. This is a false positive on your own source-built binary. This script
# excludes the repo folder from scanning and restores the quarantined file.
#
# Run it ONCE, as administrator:
#   Right-click "Developer PowerShell for VS 2022" -> Run as administrator, then:
#   powershell -ExecutionPolicy Bypass -File tools\defender_allow.ps1
# or from an elevated PowerShell:
#   Start-Process powershell -Verb RunAs -ArgumentList '-ExecutionPolicy','Bypass','-File','C:\Users\localadmin\Desktop\Hacktoberfest\tools\defender_allow.ps1'

#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# 1) Don't scan the repo (so every rebuild is fine), and trust the two executables by name.
Add-MpPreference -ExclusionPath $repo
Add-MpPreference -ExclusionProcess 'kvm_host.exe'
Add-MpPreference -ExclusionProcess 'kvm_client.exe'
Write-Host "Excluded: $repo"

# 2) Bring back whatever was already quarantined (the detection name, and the two exes by path).
$mpcmd = Join-Path $env:ProgramFiles 'Windows Defender\MpCmdRun.exe'
if (Test-Path $mpcmd) {
    & $mpcmd -Restore -Name 'Trojan:Win32/Bearfoos.B!ml' 2>&1 | Out-Null
    foreach ($exe in 'host_a\kvm_host.exe', 'client_b\kvm_client.exe') {
        & $mpcmd -Restore -FilePath (Join-Path "$repo\build\x64-release" $exe) 2>&1 | Out-Null
    }
}

# 3) Show the result.
$exe = Join-Path $repo 'build\x64-release\host_a\kvm_host.exe'
Write-Host ("kvm_host.exe present: {0}" -f (Test-Path $exe))
Write-Host "Current Defender path exclusions:"
(Get-MpPreference).ExclusionPath | ForEach-Object { "  $_" }
Write-Host "`nDone. If kvm_host.exe is still missing, rebuild it (cmake --build --preset x64-release)."
Write-Host "To report the false positive to Microsoft: https://www.microsoft.com/wdsi/filesubmission"
