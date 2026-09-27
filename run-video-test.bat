@echo off
setlocal
cd /d "%~dp0"

if not exist "bin\nw.exe" goto :missing
start "NW Video Test" "bin\nw.exe" --nwapp=tests\apps\media
exit /b 0

:missing
echo Required runtime files are missing. Run build-nw.bat first.
pause
exit /b 1
