@echo off
rem Run once on the HOST computer. Opens TCP 50500-50501 for computers on the same local network.
net session >nul 2>&1
if errorlevel 1 (
    echo Asking for administrator rights...
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -Command ^
  "Remove-NetFirewallRule -DisplayName 'KVM demo (TCP 50500-50501)' -ErrorAction SilentlyContinue;" ^
  "New-NetFirewallRule -DisplayName 'KVM demo (TCP 50500-50501)' -Direction Inbound -Protocol TCP -LocalPort 50500-50501 -RemoteAddress LocalSubnet -Profile Any -Action Allow | Out-Null;" ^
  "Write-Host 'Firewall rule added: TCP 50500-50501 from the local network.';" ^
  "Get-NetConnectionProfile | ForEach-Object { Write-Host ('  network ' + $_.Name + ': ' + $_.NetworkCategory) }"
echo.
pause
