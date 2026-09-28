@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "CHECKOUT=%~1"
if not defined CHECKOUT set "CHECKOUT=%ROOT%\thirdparty\work\extract-xiso"
for %%I in ("%CHECKOUT%") do set "CHECKOUT=%%~fI"
set "PATCH=%ROOT%\patches\extract-xiso-mojorecomp-no-ftp.patch"

pushd "%CHECKOUT%" >nul || exit /b 1
git apply --check "%PATCH%" || (
  popd >nul
  echo ERROR: MojoRecomp extract-xiso patch conflicts with this checkout.
  exit /b 1
)
git apply "%PATCH%" || (
  popd >nul
  exit /b 1
)
popd >nul
exit /b 0
