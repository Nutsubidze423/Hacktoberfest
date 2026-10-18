# The Virtual Display Driver's list of resolutions (C:\VirtualDisplayDriver\vdd_settings.xml).
#
# Desk mode gives the virtual display exactly the other PC's screen size (1:1); Sarke adds a missing
# size by itself on connection (one permission prompt). This script does it by hand, and repairs the
# list: the driver offers every resolution at each refresh rate of its <global> list too, and with too
# many (35 resolutions = ~245 modes, what an earlier version of this script added) its monitor no
# longer comes up at all - the virtual display has no modes and desk mode can't start. So the list is
# kept to at most 10 resolutions.
#
# Run as administrator (it asks), on the PC that has the Virtual Display Driver installed:
#   powershell -ExecutionPolicy Bypass -File tools\vdd_modes.ps1                  # repair a list that grew too long
#   powershell -ExecutionPolicy Bypass -File tools\vdd_modes.ps1 -Size 1920x1200  # add sizes (comma-separated)
#   powershell -ExecutionPolicy Bypass -File tools\vdd_modes.ps1 -Restore         # back to the driver's own list

param(
    [string[]]$Size = @(),
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$Size = @($Size | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Size) { $argList += @('-Size', ($Size -join ',')) }
    if ($Restore) { $argList += '-Restore' }
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

$max = 10
$settings = 'C:\VirtualDisplayDriver\vdd_settings.xml'
$backup = "$settings.bak"  # the driver's own list, saved before this script first changed it
if (-not (Test-Path $settings)) { throw "$settings not found - is the Virtual Display Driver installed?" }

$modePattern = '<width>\s*(\d+)\s*</width>\s*<height>\s*(\d+)\s*</height>'
function Get-Modes([string]$xml) {
    @([regex]::Matches($xml, $modePattern) | ForEach-Object { "$($_.Groups[1].Value)x$($_.Groups[2].Value)" })
}

$xml = [System.IO.File]::ReadAllText($settings)
$original = $xml
$nl = if ($xml.Contains("`r`n")) { "`r`n" } else { "`n" }
$modes = Get-Modes $xml

# Too many resolutions (or -Restore): back to the driver's own list, or keep only the first $max.
if ($Restore -or $modes.Count -gt $max) {
    if ((Test-Path $backup) -and (Get-Modes ([System.IO.File]::ReadAllText($backup))).Count -le $max) {
        $xml = [System.IO.File]::ReadAllText($backup)
        Write-Host "Restored the driver's own list from $backup"
    } elseif ($modes.Count -gt $max) {
        $blocks = [regex]::Matches($xml, '[ \t]*<resolution>[\s\S]*?</resolution>[ \t]*\r?\n?')
        for ($i = $blocks.Count - 1; $i -ge $max; $i--) { $xml = $xml.Remove($blocks[$i].Index, $blocks[$i].Length) }
        Write-Host "Kept the first $max of $($modes.Count) resolutions"
    }
    $nl = if ($xml.Contains("`r`n")) { "`r`n" } else { "`n" }
}

$added = @()
foreach ($mode in $Size) {
    if ($mode -notmatch '^(\d+)x(\d+)$') { Write-Warning "skipping '$mode' (expected WIDTHxHEIGHT)"; continue }
    $width = $Matches[1]; $height = $Matches[2]
    if ((Get-Modes $xml) -contains $mode) { continue }
    if ((Get-Modes $xml).Count -ge $max) { Write-Warning "not adding $mode - the list has $max resolutions already (more and the driver's monitor stops working)"; continue }
    $end = $xml.IndexOf('</resolutions>')
    if ($end -lt 0) { throw "$settings has no <resolutions> list" }
    $lineStart = $xml.LastIndexOf("`n", $end) + 1
    $xml = $xml.Insert($lineStart, "        <resolution>$nl            <width>$width</width>$nl            <height>$height</height>$nl            <refresh_rate>60</refresh_rate>$nl        </resolution>$nl")
    $added += $mode
}
if ($added) { Write-Host "Added: $($added -join ', ')" }

if ($xml -ne $original) {
    if (-not (Test-Path $backup)) { Copy-Item $settings $backup }
    [System.IO.File]::WriteAllText($settings, $xml, (New-Object System.Text.UTF8Encoding($false)))
    $devices = Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -eq 'Virtual Display Driver' }
    if (-not $devices) { throw 'Virtual Display Driver device not found' }
    foreach ($d in $devices) {
        pnputil /restart-device "$($d.InstanceId)" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false
            Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false
        }
    }
    Write-Host 'Virtual Display Driver restarted.'
    Start-Sleep -Seconds 3
} else {
    Write-Host 'Nothing to change.'
}

Write-Host "Resolutions: $((Get-Modes $xml) -join ', ')"
$monitor = Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'VDD' } | Select-Object -First 1
if ($monitor) { Write-Host "Virtual monitor: $($monitor.Status)" }
Start-Sleep -Seconds 5
