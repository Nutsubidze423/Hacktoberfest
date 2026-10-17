@echo off
rem HOST computer (A): waits for the client. Log is saved to kvm_host.log next to this file.
cd /d "%~dp0"
echo ============================================================
echo  KVM host. Tell the other person one of these IP addresses:
powershell -NoProfile -Command ^
  "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } | ForEach-Object { Write-Host ('   ' + $_.IPAddress + '   (' + $_.InterfaceAlias + ')') }"
echo ============================================================
echo.
"%~dp0kvm_host.exe" --log "%~dp0kvm_host.log"
echo.
pause
