@echo off
rem HOST computer (A): like 2_run_host.cmd, but also streams synthetic video (250 KB frames, 30 fps)
rem so the CLIENT log shows real network fps, Mbit/s and video latency. Optional: frame KB and fps.
rem   5_run_host_video_test.cmd            -> 250 KB @ 30 fps (~1080p JPEG)
rem   5_run_host_video_test.cmd 100 30     -> 100 KB @ 30 fps (~720p / lower quality)
cd /d "%~dp0"
set "KB=%~1"
set "FPS=%~2"
if "%KB%"=="" set "KB=250"
if "%FPS%"=="" set "FPS=30"
echo ============================================================
echo  KVM host + test video (%KB% KB @ %FPS% fps). IP addresses:
powershell -NoProfile -Command ^
  "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } | ForEach-Object { Write-Host ('   ' + $_.IPAddress + '   (' + $_.InterfaceAlias + ')') }"
echo ============================================================
echo.
"%~dp0kvm_host.exe" --log "%~dp0kvm_host.log" --test-video --test-video-kb %KB% --test-video-fps %FPS%
echo.
pause
