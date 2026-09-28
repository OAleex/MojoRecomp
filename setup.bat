@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "THIRD=%ROOT%\thirdparty"
set "VSVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "CLANGCL=%THIRD%\llvm22\bin\clang-cl.exe"
set "LLD=%THIRD%\llvm22\bin\lld-link.exe"

set "XENON_COMMIT=ddd128bcca99fe8bfbb99bea583c972351fa6ace"
set "XENOS_COMMIT=990d03b28a27b50277ee5d8d942e1c5f873869d1"
set "FFMPEG_COMMIT=0604b464c7cb4ebc94940cf1f324a3b26b87717c"

call :require "%VSVARS%" || exit /b 1
call :require "%CMAKE%" || exit /b 1
call :require "%CLANGCL%" || exit /b 1
call :require "%LLD%" || exit /b 1

if not exist "%THIRD%" mkdir "%THIRD%"

call :ensure_checkout XenonRecomp https://github.com/hedge-dev/XenonRecomp.git %XENON_COMMIT% || exit /b 1
call :ensure_checkout XenosRecomp https://github.com/hedge-dev/XenosRecomp.git %XENOS_COMMIT% || exit /b 1
call :ensure_source_checkout ffmpeg https://github.com/wmarti/FFmpeg.git %FFMPEG_COMMIT% || exit /b 1

call "%ROOT%\tools\apply-xenon-patches.bat" "%THIRD%\XenonRecomp-src" || exit /b 1
call "%ROOT%\tools\apply-xenos-patches.bat" "%THIRD%\XenosRecomp-src" || exit /b 1

call "%VSVARS%" >nul || exit /b 1

call :cmake_build "%THIRD%\XenonRecomp-src" "%THIRD%\XenonRecomp-src\build-win" || exit /b 1
call :cmake_build "%THIRD%\XenosRecomp-src" "%THIRD%\XenosRecomp-src\build-win" "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_PROGRAMS=OFF" || exit /b 1
call :cmake_build "%ROOT%\tools\ffmpeg-rexglue" "%THIRD%\ffmpeg-build" "-DFFMPEG_ROOT=%THIRD%\ffmpeg-src" || exit /b 1

echo MojoRecomp recompilation toolchain is ready.
exit /b 0

:require
if exist "%~1" exit /b 0
echo ERROR: Required tool is missing: %~1
exit /b 1

:ensure_checkout
set "CHECKOUT=%THIRD%\%~1-src"
if not exist "%CHECKOUT%\.git" (
  git clone --recursive "%~2" "%CHECKOUT%" || exit /b 1
)
git -C "%CHECKOUT%" fetch --tags origin || exit /b 1
git -C "%CHECKOUT%" checkout --detach "%~3" || exit /b 1
git -C "%CHECKOUT%" submodule update --init --recursive || exit /b 1
exit /b 0

:ensure_source_checkout
set "CHECKOUT=%THIRD%\%~1-src"
if not exist "%CHECKOUT%\.git" (
  git clone --filter=blob:none --no-checkout "%~2" "%CHECKOUT%" || exit /b 1
)
git -C "%CHECKOUT%" fetch --depth 1 origin "%~3" || exit /b 1
git -C "%CHECKOUT%" checkout --detach "%~3" || exit /b 1
exit /b 0

:cmake_build
set "SOURCE_DIR=%~1"
set "BUILD_DIR=%~2"
"%CMAKE%" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%CLANGCL%" -DCMAKE_CXX_COMPILER="%CLANGCL%" -DCMAKE_LINKER="%LLD%" %3 %4 || exit /b 1
"%CMAKE%" --build "%BUILD_DIR%" --parallel 8 || exit /b 1
exit /b 0
