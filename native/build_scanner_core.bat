@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%" || goto fail

REM Try vswhere first
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
)

REM If vswhere didn't find it, allow an explicit local override.
if "%VSROOT%"=="" (
    set "VSROOT=%HELIXSEED_VSROOT%"
)
if "%VSROOT%"=="" goto no_vsroot

set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%"
if errorlevel 1 goto fail

set "COMMON_CPP=/nologo /std:c++20 /O2 /Oi /Ot /GL /Gy /DNDEBUG /EHsc"
set "CORE_ARCH=/arch:AVX2"
set "NATIVE_ARCH=/arch:AVX2"
set "HAS_AVX512=0"

if /I "%SCANNER_NATIVE_ARCH%"=="AVX512" (
    set "NATIVE_ARCH=/arch:AVX512"
    set "HAS_AVX512=1"
) else (
    for /f %%I in ('powershell -NoProfile -Command "try { [int]([System.Runtime.Intrinsics.X86.Avx512F]::IsSupported) } catch { 0 }"') do set "HAS_AVX512=%%I"
    if "%HAS_AVX512%"=="1" (
        set "NATIVE_ARCH=/arch:AVX512"
    )
)

echo [scanner_core] AVX512 detection: HAS_AVX512=%HAS_AVX512%, SCANNER_NATIVE_ARCH=%SCANNER_NATIVE_ARCH%
echo [scanner_core] Building scanner_core.dll with %CORE_ARCH%
echo [scanner_core] Building scanner_native.exe with %NATIVE_ARCH%

cl %COMMON_CPP% %CORE_ARCH% /LD /DSCANNER_CORE_BUILD_DLL scanner_core.cpp /Fe:scanner_core.dll /link /LTCG /OPT:REF /OPT:ICF
if errorlevel 1 goto fail

cl %COMMON_CPP% %NATIVE_ARCH% scanner_native.cpp scanner_core.lib /Fe:scanner_native.exe /link /LTCG /OPT:REF /OPT:ICF
if errorlevel 1 goto fail

cl %COMMON_CPP% %CORE_ARCH% scanner_backend.cpp /Fe:scanner_backend.exe /link /LTCG /OPT:REF /OPT:ICF
if errorlevel 1 goto fail

echo [scanner_core] Built native\scanner_core.dll
echo [scanner_core] Built native\scanner_native.exe
echo [scanner_core] Built native\scanner_backend.exe
exit /b 0

:no_vswhere
echo [scanner_core] vswhere.exe not found. Install Visual Studio Build Tools 2022.
exit /b 1

:no_vsroot
echo [scanner_core] Visual C++ build tools not found.
exit /b 1

:no_vcvars
echo [scanner_core] vcvars64.bat not found: %VCVARS%
exit /b 1

:fail
echo [scanner_core] Build failed.
exit /b 1
