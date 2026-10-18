# Tests the installer on a blank Windows: opens Windows Sandbox (a throwaway Windows with no tools,
# gone when closed), which installs the newest dist\SarkeSetup-*.exe, checks that Sarke runs there,
# uninstalls it and checks the cleanup (tools\sandbox\check_install.ps1). The report opens inside the
# sandbox and is also written to dist\sandbox-report\report.txt here.
#
#   powershell -ExecutionPolicy Bypass -File tools\sandbox_test.ps1
#
# Windows Sandbox must be on (Windows 10/11 Pro or Enterprise, once, as admin, then restart):
#   Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sandbox = "$env:windir\System32\WindowsSandbox.exe"
if (-not (Test-Path $sandbox)) {
    throw 'Windows Sandbox is off. Turn it on once (admin PowerShell, then restart Windows): Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All'
}
$dist = Join-Path $root 'dist'
if (-not (Get-ChildItem $dist -Filter 'SarkeSetup-*.exe' -ErrorAction SilentlyContinue)) {
    throw 'No installer in dist - run tools\make_installer.ps1 first.'
}
$report = Join-Path $dist 'sandbox-report'
if (Test-Path $report) { Remove-Item $report -Recurse -Force }
New-Item -ItemType Directory -Force $report | Out-Null

# The installer folder read-only, the check script read-only, the report folder writable.
$wsb = Join-Path $env:TEMP 'sarke_sandbox.wsb'
@"
<Configuration>
  <Networking>Enable</Networking>
  <MappedFolders>
    <MappedFolder><HostFolder>$dist</HostFolder><SandboxFolder>C:\SarkeTest\dist</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$(Join-Path $root 'tools\sandbox')</HostFolder><SandboxFolder>C:\SarkeTest\tools</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$report</HostFolder><SandboxFolder>C:\SarkeTest\report</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\SarkeTest\tools\check_install.ps1</Command>
  </LogonCommand>
</Configuration>
"@ | Set-Content -Path $wsb -Encoding utf8
Start-Process $wsb
Write-Host "Windows Sandbox is starting; the test takes about a minute after its desktop appears."
Write-Host "Report: $report\report.txt"
