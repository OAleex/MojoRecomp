@echo off
setlocal EnableExtensions

set "CHECKOUT=%~1"
if not defined CHECKOUT set "CHECKOUT=%~dp0..\thirdparty\XenonRecomp-src"
for %%I in ("%CHECKOUT%") do set "CHECKOUT=%%~fI"
set "PATCH=%~dp0..\patches\xenonrecomp-mojorecomp-runtime.patch"
for %%I in ("%PATCH%") do set "PATCH=%%~fI"

git -C "%CHECKOUT%" apply --reverse --check "%PATCH%" >nul 2>nul
if not errorlevel 1 (
  echo MojoRecomp Xenon runtime patch is already applied.
  exit /b 0
)

git -C "%CHECKOUT%" apply --check "%PATCH%" || (
  echo ERROR: MojoRecomp Xenon runtime patch conflicts with this checkout.
  exit /b 1
)
git -C "%CHECKOUT%" apply "%PATCH%" || exit /b 1
exit /b 0
