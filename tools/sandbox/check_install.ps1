# Runs inside Windows Sandbox (a blank Windows with no tools): installs the newest
# dist\SarkeSetup-*.exe, checks that Sarke works there, uninstalls it, checks that it's gone, and
# writes report.txt to the mapped report folder. Started by tools\sandbox_test.ps1.

$ErrorActionPreference = 'Continue'
$dist = 'C:\SarkeTest\dist'
$report = 'C:\SarkeTest\report'
New-Item -ItemType Directory -Force $report | Out-Null
$out = Join-Path $report 'report.txt'
$results = New-Object System.Collections.Generic.List[string]
$failed = 0
function Check([bool]$ok, [string]$what) {
    $line = if ($ok) { "[ok]   $what" } else { "[FAIL] $what" }
    $script:results.Add($line)
    if (-not $ok) { $script:failed++ }
    Set-Content -Path $script:out -Value $script:results -Encoding utf8
}

$setup = Get-ChildItem $dist -Filter 'SarkeSetup-*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Check ($null -ne $setup) "installer found: $($setup.Name)"
$results.Add("Windows: $((Get-CimInstance Win32_OperatingSystem).Caption) $([Environment]::OSVersion.Version)")

# 1) Install, as a user would, but silently (Defender exclusion on, start with Windows on).
$log = Join-Path $report 'setup.log'
$p = Start-Process $setup.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS="defender,autostart"', "/LOG=`"$log`"" -Wait -PassThru
Check ($p.ExitCode -eq 0) "setup finished (exit $($p.ExitCode))"
$app = Join-Path $env:ProgramFiles 'Sarke'
foreach ($f in 'sarke.exe', 'kvm_host.exe', 'kvm_client.exe', 'sarke_driver.exe', 'assets\fonts\GoogleSans-Regular.ttf', 'driver\mttvdd.inf') {
    Check (Test-Path (Join-Path $app $f)) "installed: $f"
}
Check (Test-Path 'C:\VirtualDisplayDriver\vdd_settings.xml') 'virtual display settings in C:\VirtualDisplayDriver'
$rules = @(Get-NetFirewallRule -DisplayName 'Sarke' -ErrorAction SilentlyContinue)
Check ($rules.Count -eq 4) "firewall rules: $($rules.Count) of 4"
Check ($null -ne (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' -Name Sarke -ErrorAction SilentlyContinue)) 'starts with Windows'
Check ((Get-MpPreference).ExclusionPath -contains $app) 'Defender exclusion'
$driver = & (Join-Path $app 'sarke_driver.exe') status
$results.Add("       driver: $driver (the sandbox's display may not take it; real PCs do)")
$vdd = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object FriendlyName -eq 'Virtual Display Driver'
$results.Add("       virtual display device: $(if ($vdd) { $vdd.Status } else { 'none' })")

# 2) Start it like the Start menu does and see that it runs on its own.
Start-Process (Join-Path $app 'sarke.exe') -WorkingDirectory $app
Start-Sleep -Seconds 10
Check ($null -ne (Get-Process sarke -ErrorAction SilentlyContinue)) 'sarke.exe runs'
Check ($null -ne (Get-Process kvm_host -ErrorAction SilentlyContinue)) 'kvm_host.exe runs (the default role)'
$config = Join-Path $env:LOCALAPPDATA 'Sarke\config.json'
Check (Test-Path $config) 'config.json created in %LOCALAPPDATA%\Sarke'
$hostLog = Join-Path $env:LOCALAPPDATA 'Sarke\logs\host.log'
$text = if (Test-Path $hostLog) { Get-Content $hostLog -Raw } else { '' }
Check ($text -match 'listening on ports') 'host listens'
Check ($text -match [regex]::Escape("fonts from $app\assets\fonts")) 'fonts from the installed folder'
Check ($text -notmatch '\[error\]') 'no errors in host.log'
if (Test-Path $hostLog) { Copy-Item $hostLog (Join-Path $report 'host.log') }

# 3) Uninstall and check that nothing is left behind.
& (Join-Path $app 'sarke.exe') --quit
Start-Sleep -Seconds 4
$p = Start-Process (Join-Path $app 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
Start-Sleep -Seconds 3
Check ($p.ExitCode -eq 0) "uninstall finished (exit $($p.ExitCode))"
Check (-not (Test-Path (Join-Path $app 'sarke.exe'))) 'program files removed'
Check (@(Get-NetFirewallRule -DisplayName 'Sarke' -ErrorAction SilentlyContinue).Count -eq 0) 'firewall rules removed'
Check ($null -eq (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' -Name Sarke -ErrorAction SilentlyContinue)) 'start with Windows removed'
Check (-not ((Get-MpPreference).ExclusionPath -contains $app)) 'Defender exclusion removed'
Check ($null -eq (Get-Process sarke, kvm_host, kvm_client -ErrorAction SilentlyContinue)) 'nothing of Sarke still running'

$results.Add('')
$results.Add($(if ($failed -eq 0) { 'ALL PASSED' } else { "$failed CHECK(S) FAILED" }))
Set-Content -Path $out -Value $results -Encoding utf8
Start-Process notepad.exe $out
