# ctest fixture: opens a window for the tests that share one (window_mover_test, window_share_test, ...)
# and closes exactly that window afterwards, never one the user had open.
#
#   test_window.ps1 -Start mspaint -PidFile <file>   start it, wait for its window, write its PID
#   test_window.ps1 -Stop -PidFile <file>            end the process whose PID is in the file
param(
    [string]$Start,
    [switch]$Stop,
    [Parameter(Mandatory = $true)][string]$PidFile,
    [int]$TimeoutSeconds = 20
)
$ErrorActionPreference = 'Stop'

if ($Stop) {
    if (Test-Path $PidFile) {
        $id = [int](Get-Content $PidFile -Raw)
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($p) { Stop-Process -Id $id -Force }
        Remove-Item $PidFile -Force
    }
    exit 0
}

if (-not $Start) { Write-Error 'give -Start <program> or -Stop'; exit 2 }

# Processes of that name that were already running belong to the user: never pick them.
$name = [IO.Path]::GetFileNameWithoutExtension($Start)
$before = @(Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
Start-Process $Start | Out-Null

# On Windows 11 the Store app may run under another PID than the one Start-Process returns:
# wait for any new process of that name with a visible main window.
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    $new = Get-Process -Name $name -ErrorAction SilentlyContinue |
        Where-Object { $before -notcontains $_.Id -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($new) {
        Set-Content -Path $PidFile -Value $new.Id -Encoding ascii
        Write-Output "test_window: $Start is up (pid $($new.Id), '$($new.MainWindowTitle)')"
        Start-Sleep -Milliseconds 500  # let it finish its first paint
        exit 0
    }
    Start-Sleep -Milliseconds 200
}
Write-Error "test_window: no window from $Start within $TimeoutSeconds s"
exit 1
