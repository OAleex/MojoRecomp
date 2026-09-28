@echo off
setlocal

cd /d "%~dp0"
if errorlevel 1 exit /b 1

where node.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: Node.js is required to build a release package.
  exit /b 1
)

where tar.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: Windows tar.exe is required to create the portable ZIP archive.
  exit /b 1
)

call npm.cmd --prefix launcher run build:production
if errorlevel 1 exit /b 1

call npm.cmd --prefix launcher run package:release
if errorlevel 1 exit /b 1

echo.
echo Release candidate created under .release\
exit /b 0
