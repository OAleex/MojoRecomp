@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "THIRD=%ROOT%\thirdparty"
set "BUILD=%THIRD%\build"
set "WORK=%THIRD%\work"
set "VSVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "CLANGCL=%THIRD%\llvm22\bin\clang-cl.exe"
set "LLD=%THIRD%\llvm22\bin\lld-link.exe"

set "XENON_SOURCE=%THIRD%\XenonRecomp"
set "XENOS_SOURCE=%THIRD%\XenosRecomp"
set "FFMPEG_SOURCE=%THIRD%\FFmpeg"
set "SDL_SOURCE=%THIRD%\SDL"
set "VULKAN_SOURCE=%THIRD%\Vulkan-Headers"
set "EXTRACT_XISO_SOURCE=%THIRD%\extract-xiso"

set "XENON_WORK=%WORK%\XenonRecomp"
set "XENOS_WORK=%WORK%\XenosRecomp"
set "EXTRACT_XISO_WORK=%WORK%\extract-xiso"
set "XENON_COMMIT=ddd128bcca99fe8bfbb99bea583c972351fa6ace"
set "XENOS_COMMIT=990d03b28a27b50277ee5d8d942e1c5f873869d1"
set "FFMPEG_COMMIT=0604b464c7cb4ebc94940cf1f324a3b26b87717c"
set "SDL_COMMIT=8bf3b7215ad9fc3deb583c6a3a37c6c67f2e24e4"
set "VULKAN_COMMIT=e3b1eec08173d6b825cd3ac88c885a63b621504a"
set "EXTRACT_XISO_COMMIT=1766b46fb0638c70e13a3a429093c2c9376ce8fe"

call :require "%VSVARS%" || exit /b 1
call :require "%CMAKE%" || exit /b 1
call :require "%CLANGCL%" || exit /b 1
call :require "%LLD%" || exit /b 1

git -C "%ROOT%" submodule update --init --recursive || (
  echo ERROR: Could not initialize third-party source submodules.
  exit /b 1
)

call :require_submodule "%XENON_SOURCE%" "%XENON_COMMIT%" "XenonRecomp" || exit /b 1
call :require_submodule "%XENOS_SOURCE%" "%XENOS_COMMIT%" "XenosRecomp" || exit /b 1
call :require_submodule "%FFMPEG_SOURCE%" "%FFMPEG_COMMIT%" "FFmpeg" || exit /b 1
call :require_submodule "%SDL_SOURCE%" "%SDL_COMMIT%" "SDL" || exit /b 1
call :require_submodule "%VULKAN_SOURCE%" "%VULKAN_COMMIT%" "Vulkan-Headers" || exit /b 1
call :require_submodule "%EXTRACT_XISO_SOURCE%" "%EXTRACT_XISO_COMMIT%" "extract-xiso" || exit /b 1

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%WORK%" mkdir "%WORK%"

call :sync_source "%XENON_SOURCE%" "%XENON_WORK%" "XenonRecomp" || exit /b 1
call :sync_source "%XENOS_SOURCE%" "%XENOS_WORK%" "XenosRecomp" || exit /b 1
call :sync_source "%EXTRACT_XISO_SOURCE%" "%EXTRACT_XISO_WORK%" "extract-xiso" || exit /b 1

call "%ROOT%\tools\apply-xenon-patches.bat" "%XENON_WORK%" || exit /b 1
call "%ROOT%\tools\apply-xenos-patches.bat" "%XENOS_WORK%" || exit /b 1
call "%ROOT%\tools\apply-extract-xiso-patches.bat" "%EXTRACT_XISO_WORK%" || exit /b 1

call "%VSVARS%" >nul || exit /b 1

set "CMAKE_ARGS="
call :cmake_build "%XENON_WORK%" "%BUILD%\XenonRecomp" || exit /b 1

set CMAKE_ARGS="-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_PROGRAMS=OFF"
call :cmake_build "%XENOS_WORK%" "%BUILD%\XenosRecomp" || exit /b 1

set CMAKE_ARGS="-DFFMPEG_ROOT:PATH=%FFMPEG_SOURCE%"
call :cmake_build "%ROOT%\tools\ffmpeg-rexglue" "%BUILD%\FFmpeg" || exit /b 1

set CMAKE_ARGS="-DSDL_SHARED=OFF" "-DSDL_STATIC=ON" "-DSDL_TESTS=OFF" "-DSDL_EXAMPLES=OFF"
call :cmake_build "%SDL_SOURCE%" "%BUILD%\SDL" "SDL3-static" || exit /b 1

set CMAKE_ARGS="-DEXTRACT_XISO_ROOT:PATH=%EXTRACT_XISO_WORK%"
call :cmake_build "%ROOT%\tools\extract-xiso" "%BUILD%\extract-xiso" || exit /b 1

echo MojoRecomp recompilation toolchain is ready.
exit /b 0

:require
if exist "%~1" exit /b 0
echo ERROR: Required tool is missing: %~1
exit /b 1

:require_submodule
if not exist "%~1\.git" (
  echo ERROR: %~3 submodule is missing: %~1
  echo Run: git submodule update --init --recursive
  exit /b 1
)
set "ACTUAL_COMMIT="
for /f %%H in ('git -C "%~1" rev-parse HEAD') do set "ACTUAL_COMMIT=%%H"
if /I not "!ACTUAL_COMMIT!"=="%~2" (
  echo ERROR: %~3 is at !ACTUAL_COMMIT!, expected %~2.
  echo Run: git submodule update --init --recursive
  exit /b 1
)
for /f "delims=" %%S in ('git -C "%~1" status --porcelain') do (
  echo ERROR: %~3 submodule has local changes. Keep upstream submodules pristine.
  exit /b 1
)
exit /b 0

:sync_source
if exist "%~2" rmdir /s /q "%~2"
if exist "%~2" (
  echo ERROR: Could not reset %~3 patch work tree.
  exit /b 1
)
mkdir "%~2" || exit /b 1
robocopy "%~1" "%~2" /E /XD .git /XF .git /R:2 /W:1 /NFL /NDL /NJH /NJS /NC /NS /NP >nul
set "COPY_RESULT=!ERRORLEVEL!"
if !COPY_RESULT! GEQ 8 (
  echo ERROR: Could not prepare %~3 patch work tree.
  exit /b 1
)
git -C "%~2" init -q || exit /b 1
exit /b 0

:cmake_build
set "SOURCE_DIR=%~1"
set "BUILD_DIR=%~2"
"%CMAKE%" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%CLANGCL%" -DCMAKE_CXX_COMPILER="%CLANGCL%" -DCMAKE_LINKER="%LLD%" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded !CMAKE_ARGS! || exit /b 1
if not "%~3"=="" (
  "%CMAKE%" --build "%BUILD_DIR%" --target "%~3" --parallel 8 || exit /b 1
) else (
  "%CMAKE%" --build "%BUILD_DIR%" --parallel 8 || exit /b 1
)
exit /b 0
