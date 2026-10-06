@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
if errorlevel 1 exit /b 1
py -3 tools\build-localization-delta.py %*
exit /b %errorlevel%
