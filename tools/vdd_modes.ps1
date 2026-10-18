# Adds the common laptop and monitor resolutions to the Virtual Display Driver and restarts it, so desk
# mode can give the virtual display exactly the other PC's screen size (1:1). Without the right mode the
# virtual display falls back to a smaller one, and part of the other screen (e.g. its taskbar) is out of
# reach. Sarke also adds a missing mode by itself on connection (one permission prompt).
#
# Run once per PC, as administrator (the PC that has the Virtual Display Driver installed):
#   powershell -ExecutionPolicy Bypass -File tools\vdd_modes.ps1
#   powershell -ExecutionPolicy Bypass -File tools\vdd_modes.ps1 -Extra 3456x2234,2048x1280

param(
    [string[]]$Extra = @()
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"") + $(if ($Extra) { @('-Extra', ($Extra -join ',')) } else { @() }))
    exit
}

$settings = 'C:\VirtualDisplayDriver\vdd_settings.xml'
if (-not (Test-Path $settings)) { throw "$settings not found - is the Virtual Display Driver installed?" }

# Physical pixel sizes of common laptop panels and monitors.
$modes = @(
    '1024x768', '1280x720', '1280x800', '1280x1024', '1366x768', '1440x900', '1600x900', '1600x1200',
    '1680x1050', '1920x1080', '1920x1200', '2048x1152', '2160x1440', '2240x1400', '2256x1504', '2400x1600',
    '2496x1664', '2560x1080', '2560x1440', '2560x1600', '2736x1824', '2880x1620', '2880x1800', '2880x1920',
    '3000x2000', '3024x1964', '3072x1920', '3200x1800', '3200x2000', '3240x2160', '3440x1440', '3456x2160',
    '3840x2160', '3840x2400'
) + $Extra

$xml = [System.IO.File]::ReadAllText($settings)
$nl = if ($xml.Contains("`r`n")) { "`r`n" } else { "`n" }
$existing = @{}
foreach ($m in [regex]::Matches($xml, '<width>\s*(\d+)\s*</width>\s*<height>\s*(\d+)\s*</height>')) {
    $existing["$($m.Groups[1].Value)x$($m.Groups[2].Value)"] = $true
}
$added = @()
$block = ''
foreach ($mode in $modes) {
    if ($mode -notmatch '^(\d+)x(\d+)$') { Write-Warning "skipping '$mode' (expected WIDTHxHEIGHT)"; continue }
    if ($existing.ContainsKey($mode)) { continue }
    $existing[$mode] = $true
    $block += "        <resolution>$nl            <width>$($Matches[1])</width>$nl            <height>$($Matches[2])</height>$nl            <refresh_rate>60</refresh_rate>$nl        </resolution>$nl"
    $added += $mode
}
if ($added.Count -gt 0) {
    $end = $xml.IndexOf('</resolutions>')
    if ($end -lt 0) { throw "$settings has no <resolutions> list" }
    $lineStart = $xml.LastIndexOf("`n", $end) + 1
    Copy-Item $settings "$settings.bak" -Force
    [System.IO.File]::WriteAllText($settings, $xml.Insert($lineStart, $block), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Added $($added.Count) resolution(s): $($added -join ', ')"
} else {
    Write-Host 'All resolutions were listed already.'
}

$devices = Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -eq 'Virtual Display Driver' }
if (-not $devices) { throw 'Virtual Display Driver device not found' }
foreach ($d in $devices) {
    pnputil /restart-device "$($d.InstanceId)" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false
        Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false
    }
}
Write-Host 'Virtual Display Driver restarted - the new resolutions are available.'
Start-Sleep -Seconds 3
