@echo off
REM ============================================================================
REM build_dll.bat - Build KataGo as a DLL (shared library) on Windows
REM
REM Usage:
REM   build_dll.bat                     - Build with EIGEN (CPU) backend
REM   build_dll.bat CUDA                - Build with CUDA backend
REM   build_dll.bat OPENCL              - Build with OpenCL backend
REM   build_dll.bat EIGEN               - Build with EIGEN (CPU) backend
REM
REM Prerequisites:
REM   - Visual Studio 2019 or 2022 Build Tools
REM   - CMake (found automatically or set CMAKE_EXE below)
REM   - vcpkg (found automatically or set VCPKG_EXE below)
REM
REM Output:
REM   build_dll\katago.dll               - The shared library
REM   build_dll\katago.lib               - Import library for linking
REM   build_dll\katago_dll_smoke.exe     - C ABI verification harness
REM ============================================================================

setlocal enabledelayedexpansion

REM --- Configuration ---
REM CMAKE_EXE and VCPKG_EXE may be supplied by the caller. Otherwise prefer
REM PATH/VCPKG_ROOT and finally conventional per-user installations.
set "BUILD_DIR=build_dll"
set "BUILD_TYPE=Release"

REM --- Parse backend argument ---
set "BACKEND=EIGEN"
if not "%~1"=="" set "BACKEND=%~1"

echo.
echo ============================================================
echo  KataGo DLL Builder
echo  Backend: %BACKEND%
echo  Build type: %BUILD_TYPE%
echo ============================================================
echo.

REM --- Find Visual Studio ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio Build Tools.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo ERROR: Visual Studio installation not found.
    exit /b 1
)
echo Found Visual Studio at: !VS_PATH!

REM --- Setup VS environment ---
set "VSDEVCMD=!VS_PATH!\Common7\Tools\VsDevCmd.bat"
if not exist "!VSDEVCMD!" (
    echo ERROR: VsDevCmd.bat not found at !VSDEVCMD!
    exit /b 1
)
echo Setting up Visual Studio environment...
call "!VSDEVCMD!" -arch=x64 -no_logo

REM --- Find CMake ---
if not defined CMAKE_EXE (
    where cmake >nul 2>&1
    if not errorlevel 1 set "CMAKE_EXE=cmake"
)
if not defined CMAKE_EXE if exist "%USERPROFILE%\opt\cmake\bin\cmake.exe" (
    set "CMAKE_EXE=%USERPROFILE%\opt\cmake\bin\cmake.exe"
)
if not defined CMAKE_EXE (
    echo ERROR: cmake not found. Put it on PATH or set CMAKE_EXE.
    exit /b 1
)
echo Using CMake: %CMAKE_EXE%

REM --- Find/install vcpkg dependencies ---
if not defined VCPKG_EXE if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\vcpkg.exe" (
    set "VCPKG_EXE=%VCPKG_ROOT%\vcpkg.exe"
)
if not defined VCPKG_EXE (
    for /f "delims=" %%i in ('where vcpkg.exe 2^>nul') do if not defined VCPKG_EXE set "VCPKG_EXE=%%i"
)
if not defined VCPKG_EXE if exist "%USERPROFILE%\opt\vcpkg\vcpkg.exe" (
    set "VCPKG_EXE=%USERPROFILE%\opt\vcpkg\vcpkg.exe"
)
if defined VCPKG_EXE if exist "%VCPKG_EXE%" (
    echo.
    echo Installing dependencies via vcpkg...
    "%VCPKG_EXE%" install zlib:x64-windows --no-print-usage
    if errorlevel 1 (
        echo WARNING: Failed to install zlib via vcpkg.
    )
    if /i "%BACKEND%"=="EIGEN" (
        "%VCPKG_EXE%" install eigen3:x64-windows --no-print-usage
        if errorlevel 1 (
            echo WARNING: Failed to install eigen3 via vcpkg.
        )
    )
    if /i "%BACKEND%"=="OPENCL" (
        "%VCPKG_EXE%" install opencl:x64-windows --no-print-usage
        if errorlevel 1 (
            echo WARNING: Failed to install opencl via vcpkg.
        )
    )
    for %%i in ("%VCPKG_EXE%") do set "VCPKG_DIR=%%~dpi"
    set "VCPKG_TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=!VCPKG_DIR!scripts\buildsystems\vcpkg.cmake"
) else (
    echo WARNING: vcpkg not found. Put it on PATH, set VCPKG_ROOT/VCPKG_EXE,
    echo          or install dependencies manually.
    set "VCPKG_TOOLCHAIN="
)

REM --- Configure ---
echo.
echo Configuring CMake...
if exist "%BUILD_DIR%" (
    echo Removing old build directory...
    rmdir /s /q "%BUILD_DIR%"
)

set "CMAKE_ARGS=-B %BUILD_DIR%"
set "CMAKE_ARGS=%CMAKE_ARGS% -G Ninja"
set "CMAKE_ARGS=%CMAKE_ARGS% -DCMAKE_BUILD_TYPE=%BUILD_TYPE%"
set "CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_AS_DLL=1"
set "CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_DLL_SMOKE=1"
set "CMAKE_ARGS=%CMAKE_ARGS% -DNO_GIT_REVISION=1"
set "CMAKE_ARGS=%CMAKE_ARGS% -DUSE_BACKEND=%BACKEND%"
set "CMAKE_ARGS=%CMAKE_ARGS% -DUSE_AVX2=1"

"%CMAKE_EXE%" %CMAKE_ARGS% %VCPKG_TOOLCHAIN%
if errorlevel 1 (
    echo.
    echo ERROR: CMake configuration failed.
    echo.
    echo If zlib was not found, install it:
    echo   %VCPKG_EXE% install zlib:x64-windows
    echo   Then re-run this script.
    echo.
    echo If Eigen3 was not found, install it:
    echo   %VCPKG_EXE% install eigen3:x64-windows
    echo   Then re-run this script.
    exit /b 1
)

REM --- Build ---
echo.
echo Building KataGo DLL...
"%CMAKE_EXE%" --build %BUILD_DIR% --config %BUILD_TYPE% -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo.
    echo ERROR: Build failed.
    exit /b 1
)

REM --- Verify the public header and import library with a pure-C consumer ---
if exist "%BUILD_DIR%\katago_dll_c_abi_smoke.exe" (
    echo.
    echo Running pure-C ABI smoke test...
    "%BUILD_DIR%\katago_dll_c_abi_smoke.exe"
    if errorlevel 1 (
        echo ERROR: Pure-C ABI smoke test failed.
        exit /b 1
    )
)

REM --- Report results ---
echo.
echo ============================================================
echo  BUILD SUCCESSFUL!
echo ============================================================
echo.
echo Output files:
if exist "%BUILD_DIR%\katago.dll" (
    echo   DLL:    %CD%\%BUILD_DIR%\katago.dll
    echo   LIB:    %CD%\%BUILD_DIR%\katago.lib
    echo   C ABI:  %CD%\%BUILD_DIR%\katago_dll_c_abi_smoke.exe
    echo   SMOKE:  %CD%\%BUILD_DIR%\katago_dll_smoke.exe
) else if exist "%BUILD_DIR%\%BUILD_TYPE%\katago.dll" (
    echo   DLL:    %CD%\%BUILD_DIR%\%BUILD_TYPE%\katago.dll
    echo   LIB:    %CD%\%BUILD_DIR%\%BUILD_TYPE%\katago.lib
    echo   C ABI:  %CD%\%BUILD_DIR%\%BUILD_TYPE%\katago_dll_c_abi_smoke.exe
    echo   SMOKE:  %CD%\%BUILD_DIR%\%BUILD_TYPE%\katago_dll_smoke.exe
) else (
    echo   (Check %BUILD_DIR% for output files)
    dir /s /b "%BUILD_DIR%\katago.*" 2>nul
)
echo.
echo Header:   %CD%\lib\katago_api.h
echo.
echo To use the DLL in your project:
echo   1. Copy katago.dll next to your executable
echo   2. Link against katago.lib (MSVC) or katago.dll (MinGW)
echo   3. Include katago_api.h (in the lib\ folder) in your source code
echo.

endlocal
