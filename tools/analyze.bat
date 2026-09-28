@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "ANALYZE=%ROOT%\thirdparty\XenonRecomp-src\build-win\XenonAnalyse\XenonAnalyse.exe"
set "XEX=%ROOT%\game\default.xex"
set "OUT=%ROOT%\config\CrashOfTheTitans_switch_tables.toml"

if not exist "%ANALYZE%" (
  echo ERROR: XenonAnalyse is not built: %ANALYZE%
  exit /b 1
)
if not exist "%XEX%" (
  echo ERROR: Game XEX is missing: %XEX%
  exit /b 1
)

"%ANALYZE%" "%XEX%" "%OUT%" || exit /b 1
for /f %%C in ('findstr /b /l /c:"[[switch]]" "%OUT%" ^| find /c /v ""') do set "COUNT=%%C"
echo Detected %COUNT% switch tables -^> %OUT%
exit /b 0
