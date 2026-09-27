@echo off
rem Build the nw host (bin\nw.exe) with zig -- same reason as ..\build-zig.bat:
rem the MSVC CRT libs on this machine are corrupt, cl always fails to link.
rem
rem Output: nw\bin\nw.exe (+ js\api\*.js / js\nw_node.js, the exe prefers the
rem copies sitting next to it). Kernel: miniblink_x64.dll first, then mb* fallback, looked up next
rem to the exe -> exe\miniblink -> PATH, or forced with --nw-kernel=<path>.
rem Layout: hand-written C++ in src\, generated C++ in generated\.
rem
rem NOTE: keep this file pure ASCII. cmd.exe reads a .bat as bytes in the OEM
rem codepage (936 here), so UTF-8 Chinese turns into garbage that can swallow
rem following characters and break parsing ("The syntax of the command is
rem incorrect"). English-only text is immune.
setlocal
cd /d "%~dp0"

set "ZIG="
for %%D in (..\.toolchain-fresh ..\.toolchain-fixed ..\.toolchain ..\NativeMediaBridge\.toolchain-fixed ..\NativeMediaBridge\.toolchain) do (
    if not defined ZIG if exist "%%D\zig-x86_64-windows-0.15.2\zig.exe" set "ZIG=%%D\zig-x86_64-windows-0.15.2\zig.exe"
)
if not defined ZIG for /f "delims=" %%P in ('where zig 2^>nul') do if not defined ZIG set "ZIG=%%P"
if not defined ZIG (
    echo [build-nw] zig.exe not found
    exit /b 1
)
echo [build-nw] using %ZIG%

if not exist bin mkdir bin
if not exist generated mkdir generated

rem Embedded scripts: prefer baking them into the exe (single-file distribution).
rem If generation fails we fall back to the empty stub, and then js\api\*.js /
rem js\nw_node.js must sit next to the exe.
set "SCRIPT_SRC=generated\nw_script_generated.cpp"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\embed-js.ps1" -ApiDir "js\api" -NodeJs "js\nw_node.js" -Output "generated\nw_script_generated.cpp"
if errorlevel 1 (
    echo [build-nw] embed-js failed, falling back to src\nw_script_stub.cpp
    set "SCRIPT_SRC=src\nw_script_stub.cpp"
)

"%ZIG%" c++ -target x86_64-windows-gnu -std=c++17 -O2 -Isrc ^
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE ^
    src\nw_main.cpp src\nw_host.cpp src\nw_bridge.cpp src\nw_kernel.cpp src\nw_package.cpp src\nw_rpc.cpp %SCRIPT_SRC% ^
    -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -ladvapi32 -lkernel32 -lwinspool -lws2_32 -lversion -lm ^
    -Wl,--subsystem,windows ^
    -o bin\nw.exe
if errorlevel 1 (
    echo [build-nw] compile failed
    exit /b 1
)

rem API scripts are embedded into nw.exe above; do not leave a bin\api\ copy.
rem Removing the directory also cleans installations produced by older builds.
if exist bin\api rmdir /s /q bin\api
rem Stale single-file copy from before the api\ split: the host no longer reads it,
rem so leaving it around would just be a second, wrong source of truth.
if exist bin\nw_api.js del /q bin\nw_api.js >nul
if exist bin\nw_node.js del /q bin\nw_node.js >nul

rem miniblink_x64.dll is the bridge-mode default kernel and is searched first.
rem If absent, the host may fall back to an explicitly selected alternate kernel.


echo [build-nw] BUILD-OK
dir /TW bin\nw.exe | findstr /i nw.exe
