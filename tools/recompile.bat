@echo off
setlocal EnableExtensions
python "%~dp0recompile.py"
exit /b %ERRORLEVEL%
