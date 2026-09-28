@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "VSVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "CLANGCL=%ROOT%\thirdparty\llvm22\bin\clang-cl.exe"
set "BUILD=%ROOT%\runtime\build-smoke"

if not exist "%VSVARS%" (
  echo ERROR: Required tool is missing: %VSVARS%
  exit /b 1
)
if not exist "%CMAKE%" (
  echo ERROR: Required tool is missing: %CMAKE%
  exit /b 1
)
if not exist "%CLANGCL%" (
  echo ERROR: Required tool is missing: %CLANGCL%
  exit /b 1
)

call "%VSVARS%" >nul || exit /b 1
"%CMAKE%" -S "%ROOT%\runtime" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%CLANGCL%" -DCMAKE_CXX_COMPILER="%CLANGCL%" || exit /b 1
"%CMAKE%" --build "%BUILD%" --parallel 8 || exit /b 1

"%BUILD%\MojoRecompSmoke.exe"
if errorlevel 1 (
  echo ERROR: MojoRecompSmoke failed with exit code %ERRORLEVEL%.
  exit /b %ERRORLEVEL%
)
exit /b 0
