@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "CHECKOUT=%~1"
if not defined CHECKOUT set "CHECKOUT=%ROOT%\thirdparty\work\XenosRecomp"
for %%I in ("%CHECKOUT%") do set "CHECKOUT=%%~fI"
set "PATCH=%~dp0..\patches\xenosrecomp-mojorecomp-runtime.patch"
for %%I in ("%PATCH%") do set "PATCH=%%~fI"
pushd "%CHECKOUT%" >nul || exit /b 1
git apply --check "%PATCH%" || (
  popd >nul
  echo ERROR: MojoRecomp Xenos runtime patch conflicts with this checkout.
  exit /b 1
)
git apply "%PATCH%" || (
  popd >nul
  exit /b 1
)
popd >nul
exit /b 0
