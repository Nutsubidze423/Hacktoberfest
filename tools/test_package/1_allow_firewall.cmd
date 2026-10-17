@echo off
rem Run once on BOTH computers. Opens TCP 50500-50502 and UDP 50503 (discovery) for the local network.
net session >nul 2>&1
if errorlevel 1 (
    echo Asking for administrator rights...
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -Command ^
  "Remove-NetFirewallRule -DisplayName 'KVM demo (TCP 50500-50502)' -ErrorAction SilentlyContinue;" ^
  "New-NetFirewallRule -DisplayName 'KVM demo (TCP 50500-50502)' -Direction Inbound -Protocol TCP -LocalPort 50500-50502 -RemoteAddress LocalSubnet -Profile Any -Action Allow | Out-Null;" ^
  "Remove-NetFirewallRule -DisplayName 'KVM demo discovery (UDP 50503)' -ErrorAction SilentlyContinue;" ^
  "New-NetFirewallRule -DisplayName 'KVM demo discovery (UDP 50503)' -Direction Inbound -Protocol UDP -LocalPort 50503 -RemoteAddress LocalSubnet -Profile Any -Action Allow | Out-Null;" ^
  "Write-Host 'Firewall rule added: TCP 50500-50502 from the local network.';" ^
  "Write-Host 'Firewall rule added: UDP 50503 (find each other without typing an IP).';" ^
  "Get-NetConnectionProfile | ForEach-Object { Write-Host ('  network ' + $_.Name + ': ' + $_.NetworkCategory) }"
echo.
pause
