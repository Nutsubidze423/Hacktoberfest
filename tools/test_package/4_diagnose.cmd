@echo off
rem Run on the CLIENT computer if it cannot connect. Prints what is wrong.
cd /d "%~dp0"
set "HOST_IP=%~1"
if "%HOST_IP%"=="" set /p "HOST_IP=Host IP address: "
echo.
echo This computer:
powershell -NoProfile -Command ^
  "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } | ForEach-Object { Write-Host ('   ' + $_.IPAddress + '/' + $_.PrefixLength + '   (' + $_.InterfaceAlias + ')') };" ^
  "Get-NetConnectionProfile | ForEach-Object { Write-Host ('   network ' + $_.Name + ': ' + $_.NetworkCategory) }"
echo.
echo Testing %HOST_IP% port 50500 (takes up to 20 s)...
powershell -NoProfile -Command ^
  "$r = Test-NetConnection -ComputerName '%HOST_IP%' -Port 50500 -WarningAction SilentlyContinue;" ^
  "if ($r.TcpTestSucceeded) { Write-Host '   OK: port 50500 is reachable. Run 3_run_client.cmd again.' -ForegroundColor Green }" ^
  "elseif ($r.PingSucceeded) { Write-Host '   Host answers ping, but port 50500 is closed: is 2_run_host.cmd running there? Did you run 1_allow_firewall.cmd there?' -ForegroundColor Yellow }" ^
  "else { Write-Host '   Host not reachable: wrong IP, different Wi-Fi, or the network blocks devices from seeing each other (guest Wi-Fi / AP isolation). Ping may also just be blocked by the host firewall.' -ForegroundColor Red }"
echo.
pause
