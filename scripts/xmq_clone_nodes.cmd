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
set "XMQ_CLONE_ROOT=%root%"
set "XMQ_CLONE_DEFAULT=%root%\xmq\xmq_server.conf"
set "XMQ_CLONE_SOURCE=%XMQ_CLONE_DEFAULT%"
if not "%~2"=="" set "XMQ_CLONE_SOURCE=%root%\%~2\xmq_server.conf"
if /I "%~x2"==".conf" (
    set "XMQ_CLONE_SOURCE=%~f2"
    for %%D in ("%~dp2..") do set "root=%%~fD"
)
if exist "%root%\xmq1\xmq_server.conf" goto clone
rem CIM exposes each process's command line, unlike tasklist's executable-only filter.
powershell.exe -NoProfile -NonInteractive -Command ^
 "$ErrorActionPreference='Stop'; try { $source=[IO.Path]::GetFullPath($env:XMQ_CLONE_SOURCE); $pattern='(?:^|\s)(?:-c|--configuration-file)(?:\s+|=)(?:\x22([^\x22]+)\x22|(\S+))'; foreach ($p in (Get-CimInstance Win32_Process -Filter 'Name=''xmq_server.exe''')) { if (-not $p.CommandLine) { throw 'Cannot inspect XMQ process command line' }; $m=[regex]::Match($p.CommandLine,$pattern); $file=$env:XMQ_CLONE_DEFAULT; if ($m.Success) { $file=$m.Groups[1].Value; if (-not $file) { $file=$m.Groups[2].Value }; if ((-not [IO.Path]::GetDirectoryName($file)) -and (-not [IO.Path]::GetExtension($file))) { $file=Join-Path (Join-Path $env:XMQ_CLONE_ROOT $file) 'xmq_server.conf' } }; if ([string]::Equals([IO.Path]::GetFullPath($file),$source,[StringComparison]::OrdinalIgnoreCase)) { Write-Output ('Source configuration is used by PID '+$p.ProcessId); exit 1 } }; exit 0 } catch { Write-Error $_; exit 2 }"
if errorlevel 2 (
    echo Cannot inspect running XMQ configurations. No configurations were moved. 1>&2
    exit /b 1
)
if errorlevel 1 goto running

:clone
if "%~2"=="" (
    "%server%" --clone-nodes "%count%"
) else (
    "%server%" --clone-nodes "%count%" -c "%~2"
)
exit /b %errorlevel%
:running
echo Stop only the xmq_server.exe process using "%XMQ_CLONE_SOURCE%" before moving it to xmq1. 1>&2
exit /b 1
:usage
echo Usage: %~nx0 N [source\xmq_server.conf] 1>&2
exit /b 2
