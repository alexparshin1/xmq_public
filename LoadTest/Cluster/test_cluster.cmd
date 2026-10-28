@echo off
rem Native Windows entry point for the test cluster. Set CLUSTER_CONTROL_HOST first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0test_cluster.ps1" %*
exit /b %ERRORLEVEL%
