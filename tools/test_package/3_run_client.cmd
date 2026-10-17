@echo off
rem CLIENT computer (B): connects to the host. Log is saved to kvm_client.log next to this file.
cd /d "%~dp0"
set "HOST_IP=%~1"
if "%HOST_IP%"=="" set /p "HOST_IP=Host IP address: "
if "%HOST_IP%"=="" (
    echo No IP given.
    pause
    exit /b 1
)
echo.
"%~dp0kvm_client.exe" --host %HOST_IP% --log "%~dp0kvm_client.log"
echo.
pause
