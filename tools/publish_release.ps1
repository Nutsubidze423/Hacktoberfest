# Publishes the newest dist\SarkeSetup-*.exe to Supabase Storage, so anyone can download and install
# Sarke (the one installer has everything: the apps, the virtual display driver, firewall rules):
#
#   <SUPABASE_URL>/storage/v1/object/public/releases/SarkeSetup.exe              always the newest
#   <SUPABASE_URL>/storage/v1/object/public/releases/SarkeSetup-<version>.exe    this version, kept
#   <SUPABASE_URL>/storage/v1/object/public/releases/latest.json                 version, links, SHA-256
#
# The project's URL and service_role key come from the environment, never from the repo
# (Supabase dashboard -> Project Settings -> API):
#   $env:SUPABASE_URL = 'https://<project>.supabase.co'
#   $env:SUPABASE_SERVICE_ROLE_KEY = '<service_role key>'
#   powershell -ExecutionPolicy Bypass -File tools\publish_release.ps1 [-Version 1.0.1.0]
#       [-File <path>\SarkeSetup-1.1.0.0.exe]   an installer from anywhere instead of dist
#       [-Prune]                                then delete the older SarkeSetup-<version>.exe
# The public bucket "releases" is created on the first run.

param(
    [string]$Bucket = 'releases',
    [string]$Version = '',  # e.g. 1.0.1.0; default: the newest installer in dist
    [string]$File = '',     # e.g. $HOME\Desktop\SarkeSetup-1.1.0.0.exe (someone else built it)
    [switch]$Prune          # after a good upload: only this version stays (+ SarkeSetup.exe, latest.json)
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$url = $env:SUPABASE_URL
$key = $env:SUPABASE_SERVICE_ROLE_KEY
if (-not $url -or -not $key) { throw 'Set $env:SUPABASE_URL and $env:SUPABASE_SERVICE_ROLE_KEY first (Project Settings -> API).' }
$url = $url.TrimEnd('/')

if ($File) {
    $setup = Get-Item $File
    if ($setup.Name -notmatch '^SarkeSetup-\d+(\.\d+)+\.exe$') { throw "$($setup.Name): expected SarkeSetup-<version>.exe" }
    $product = "$($setup.VersionInfo.ProductName)".Trim()  # Inno Setup pads it with spaces
    if ($product -ne 'Sarke') { throw "$($setup.Name) is not a Sarke installer (product '$product')" }
} else {
    $setup = Get-ChildItem (Join-Path $root 'dist') -Filter 'SarkeSetup-*.exe' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^SarkeSetup-\d+(\.\d+)+\.exe$' -and (-not $Version -or $_.Name -eq "SarkeSetup-$Version.exe") } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $setup) { throw "No installer$(if ($Version) { " $Version" }) in dist - run tools\make_installer.ps1 first." }
}
$version = $setup.BaseName -replace '^SarkeSetup-', ''
$sha256 = (Get-FileHash $setup.FullName -Algorithm SHA256).Hash.ToLower()
Write-Host "Publishing $($setup.Name) ($([math]::Round($setup.Length / 1MB, 1)) MB, SHA-256 $sha256)"

$auth = @{ Authorization = "Bearer $key"; apikey = $key }

# The bucket: public reads, only installers and the manifest, nothing big.
try {
    Invoke-RestMethod -Method Get -Uri "$url/storage/v1/bucket/$Bucket" -Headers $auth | Out-Null
} catch {
    $body = @{ id = $Bucket; name = $Bucket; public = $true; file_size_limit = 52428800 } | ConvertTo-Json
    Invoke-RestMethod -Method Post -Uri "$url/storage/v1/bucket" -Headers $auth -ContentType 'application/json' -Body $body | Out-Null
    Write-Host "Created the public bucket '$Bucket'."
}

function Upload([string]$path, [string]$name, [string]$type) {
    $headers = $auth + @{ 'x-upsert' = 'true'; 'cache-control' = 'max-age=60' }
    Invoke-RestMethod -Method Post -Uri "$url/storage/v1/object/$Bucket/$name" -Headers $headers -ContentType $type -InFile $path | Out-Null
}

$public = "$url/storage/v1/object/public/$Bucket"
Upload $setup.FullName "SarkeSetup-$version.exe" 'application/octet-stream'
Upload $setup.FullName 'SarkeSetup.exe' 'application/octet-stream'

$manifest = Join-Path $env:TEMP 'sarke_latest.json'
$json = [ordered]@{
    version   = $version
    file      = "SarkeSetup-$version.exe"
    url       = "$public/SarkeSetup-$version.exe"
    latest    = "$public/SarkeSetup.exe"
    sha256    = $sha256
    size      = $setup.Length
    published = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
} | ConvertTo-Json
# UTF-8 without a BOM: Windows PowerShell's "utf8" writes one, and JSON readers may choke on it.
[System.IO.File]::WriteAllText($manifest, $json, (New-Object System.Text.UTF8Encoding($false)))
Upload $manifest 'latest.json' 'application/json'

# Check what people will get.
$check = Invoke-WebRequest -Method Head -Uri "$public/SarkeSetup.exe" -UseBasicParsing
Write-Host ''
Write-Host "Download link (always the newest): $public/SarkeSetup.exe"
Write-Host "This version:                      $public/SarkeSetup-$version.exe"
Write-Host "Manifest:                          $public/latest.json"
Write-Host "Served size: $($check.Headers['Content-Length']) bytes (local $($setup.Length))"

# Older versions: only once the new one is really served, so the link never points at nothing.
if ($Prune) {
    if ([long]$check.Headers['Content-Length'] -ne $setup.Length) { throw 'The served installer differs from this one - not deleting anything.' }
    $list = @{ prefix = ''; limit = 1000; offset = 0 } | ConvertTo-Json
    $objects = Invoke-RestMethod -Method Post -Uri "$url/storage/v1/object/list/$Bucket" -Headers $auth -ContentType 'application/json' -Body $list
    $old = @($objects | Where-Object { $_.name -match '^SarkeSetup-\d+(\.\d+)+\.exe$' -and $_.name -ne "SarkeSetup-$version.exe" })
    Write-Host ''
    if (-not $old) { Write-Host 'No older versions to delete.' }
    foreach ($o in $old) {
        Invoke-RestMethod -Method Delete -Uri "$url/storage/v1/object/$Bucket/$($o.name)" -Headers $auth | Out-Null
        Write-Host "Deleted $($o.name)"
    }
}
