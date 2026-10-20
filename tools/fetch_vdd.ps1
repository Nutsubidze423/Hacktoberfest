# Fetches the Virtual Display Driver package the installer ships (installer\vdd: MttVDD.dll,
# MttVDD.cat, mttvdd.inf) from its upstream release, instead of keeping the binaries in git.
#
# Pinned: VirtualDrivers/Virtual-Display-Driver release 25.7.23, MttVDD 11.30.4.434 (MIT), the
# x64 package (upstream names its folder "x86", but the .inf is NTamd64). The zip and every file
# are checked against the SHA-256 below; anything else stops the script and nothing is written.
# installer\vdd\LICENSE.txt and vdd_settings.xml (Sarke's own mode list) stay in git.
#
#   powershell -ExecutionPolicy Bypass -File tools\fetch_vdd.ps1           # once after cloning
#   powershell -ExecutionPolicy Bypass -File tools\fetch_vdd.ps1 -Force    # download again
#
# tools\make_installer.ps1 runs it. To move to a newer driver, change the URL and the hashes below
# together, and test desk mode on two PCs.
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$url = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip'
$zipSha256 = 'E24210692B442B39AF763536330CE78B423F19342B7A7792C26DE3944E418B3A'
# Name in the zip -> name in installer\vdd, SHA-256.
$files = @(
    @{ From = 'VirtualDisplayDriver/MttVDD.dll'; To = 'MttVDD.dll'; Sha256 = 'C9CA837F57A98FBD43BC416A7F535A95843626E7759EAF85CF0CD7CE334DBB05' },
    @{ From = 'VirtualDisplayDriver/mttvdd.cat'; To = 'MttVDD.cat'; Sha256 = '08A0093FC9B2E32B287A6F8A77CA4DE0A31830D29FC33D2B13A918DC859468F6' },
    @{ From = 'VirtualDisplayDriver/MttVDD.inf'; To = 'mttvdd.inf'; Sha256 = '550D211FE481E74DFE3F9D724ED78BE48B3A9113405965D683D9373E8D672F5D' }
)

$target = Join-Path (Split-Path $PSScriptRoot -Parent) 'installer\vdd'

function Test-Present {
    foreach ($f in $files) {
        $path = Join-Path $target $f.To
        if (-not (Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash -ne $f.Sha256) { return $false }
    }
    return $true
}

if (-not $Force -and (Test-Present)) {
    Write-Host "fetch_vdd: installer\vdd already has the pinned driver (11.30.4.434)"
    exit 0
}

# A fresh, empty folder for the download: nothing from the archive is run, only hashed and copied.
$work = Join-Path ([IO.Path]::GetTempPath()) ("sarke_vdd_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $zip = Join-Path $work 'vdd.zip'
    Write-Host "fetch_vdd: downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($actual -ne $zipSha256) { throw "fetch_vdd: the download's SHA-256 is $actual, expected $zipSha256 - not using it" }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $staged = @()
        foreach ($f in $files) {
            $entry = $archive.Entries | Where-Object { $_.FullName -eq $f.From } | Select-Object -First 1
            if (-not $entry) { throw "fetch_vdd: $($f.From) is not in the archive" }
            $out = Join-Path $work $f.To
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $out, $true)
            $hash = (Get-FileHash $out -Algorithm SHA256).Hash
            if ($hash -ne $f.Sha256) { throw "fetch_vdd: $($f.From) has SHA-256 $hash, expected $($f.Sha256)" }
            $staged += $f
        }
    } finally {
        $archive.Dispose()
    }

    # Every file checked: only now write into installer\vdd.
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    foreach ($f in $staged) { Copy-Item (Join-Path $work $f.To) (Join-Path $target $f.To) -Force }
    Write-Host "fetch_vdd: installer\vdd has MttVDD 11.30.4.434 (release 25.7.23), hashes checked"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
