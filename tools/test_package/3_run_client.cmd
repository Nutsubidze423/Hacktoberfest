@echo off
rem CLIENT computer (B): finds the host on the local network by itself and connects.
rem Optional: 3_run_client.cmd <host IP>  - skip the search and use that IP.
rem Log is saved to kvm_client.log next to this file.
cd /d "%~dp0"
set "HOST_IP=%~1"
echo.
if "%HOST_IP%"=="" (
    "%~dp0kvm_client.exe" --log "%~dp0kvm_client.log"
) else (
    "%~dp0kvm_client.exe" --host %HOST_IP% --log "%~dp0kvm_client.log"
)
echo.
pause
