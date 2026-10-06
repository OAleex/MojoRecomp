@echo off
setlocal EnableExtensions DisableDelayedExpansion

cd /d "%~dp0"
if errorlevel 1 exit /b 1

where node.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: Node.js is required to build a release package.
  exit /b 1
)

where tar.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: Windows tar.exe is required to create the runtime and source archives.
  exit /b 1
)

set "MOJORECOMP_RELEASE_VERSION="
set "MOJORECOMP_SUITE_VERSION="
for /f "tokens=2 delims==" %%V in ('findstr /b /c:"launcher = " version.toml') do (
  for /f "tokens=* delims= " %%W in ("%%V") do set "MOJORECOMP_RELEASE_VERSION=%%~W"
)
for /f "tokens=2 delims==" %%V in ('findstr /b /c:"suite = " version.toml') do (
  for /f "tokens=* delims= " %%W in ("%%V") do set "MOJORECOMP_SUITE_VERSION=%%~W"
)

if not defined MOJORECOMP_RELEASE_VERSION (
  echo ERROR: Could not read launcher version from version.toml.
  exit /b 1
)
if not defined MOJORECOMP_SUITE_VERSION (
  echo ERROR: Could not read suite version from version.toml.
  exit /b 1
)
if not "%MOJORECOMP_RELEASE_VERSION%"=="%MOJORECOMP_SUITE_VERSION%" (
  echo ERROR: Launcher version %MOJORECOMP_RELEASE_VERSION% does not match suite version %MOJORECOMP_SUITE_VERSION%.
  exit /b 1
)

if defined MOJORECOMP_RELEASE_SIGNING_KEY (
  if not exist "%MOJORECOMP_RELEASE_SIGNING_KEY%" (
    echo ERROR: Release signing key not found: %MOJORECOMP_RELEASE_SIGNING_KEY%
    exit /b 1
  )
) else (
  if not exist "release-signing-private.pem" (
    echo ERROR: Release signing key not found: %CD%\release-signing-private.pem
    exit /b 1
  )
)

set "MOJORECOMP_RELEASE_TAG=v%MOJORECOMP_RELEASE_VERSION%"
set "MOJORECOMP_RELEASE_REPOSITORY=https://github.com/OAleex/MojoRecomp"

if not defined MOJORECOMP_UPDATE_BASE_URL (
  set "MOJORECOMP_UPDATE_BASE_URL=%MOJORECOMP_RELEASE_REPOSITORY%/releases/download/%MOJORECOMP_RELEASE_TAG%"
)
if not defined MOJORECOMP_UPDATE_NOTES_URL (
  set "MOJORECOMP_UPDATE_NOTES_URL=%MOJORECOMP_RELEASE_REPOSITORY%/releases/tag/%MOJORECOMP_RELEASE_TAG%"
)

echo Preparing MojoRecomp release %MOJORECOMP_RELEASE_VERSION% ^(%MOJORECOMP_RELEASE_TAG%^)...
echo Update assets: %MOJORECOMP_UPDATE_BASE_URL%
echo Release notes: %MOJORECOMP_UPDATE_NOTES_URL%
echo.

if /i "%~1"=="--show-config" exit /b 0

call build-smoke.bat
if errorlevel 1 exit /b 1

call npm.cmd --prefix launcher run build:production
if errorlevel 1 exit /b 1

call npm.cmd --prefix launcher run package:release
if errorlevel 1 exit /b 1

echo.
echo Release candidate created under .release\
exit /b 0
