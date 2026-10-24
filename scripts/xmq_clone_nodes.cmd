@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if not "%~3"=="" goto usage
set "count=%~1"
for /f "delims=0123456789" %%A in ("%count%") do goto usage
rem The server validates count and all ports before changing any files.
set "server=%~dp0xmq_server.exe"
if defined XMQ_SERVER set "server=%XMQ_SERVER%"
if not exist "%server%" (
    echo xmq_server.exe was not found beside this script. Set XMQ_SERVER to its full path. 1>&2
    exit /b 1
)
for /f "delims=" %%R in ('""%server%" --configuration-root"') do set "root=%%R"
if /I "%~x2"==".conf" for %%D in ("%~dp2..") do set "root=%%~fD"
if exist "%root%\xmq1\xmq_server.conf" goto clone
tasklist /FI "IMAGENAME eq xmq_server.exe" /NH >nul 2>&1
if errorlevel 1 (
    echo Cannot check running XMQ servers. 1>&2
    exit /b 1
)
for /f "delims=" %%P in ('tasklist /FI "IMAGENAME eq xmq_server.exe" /NH 2^>nul ^| findstr /I /C:"xmq_server.exe"') do goto running
:clone
if "%~2"=="" (
    "%server%" --clone-nodes "%count%"
) else (
    "%server%" --clone-nodes "%count%" -c "%~2"
)
exit /b %errorlevel%
:running
echo Stop all xmq_server.exe processes before moving and cloning configurations. 1>&2
exit /b 1
:usage
echo Usage: %~nx0 N [source\xmq_server.conf] 1>&2
exit /b 2
