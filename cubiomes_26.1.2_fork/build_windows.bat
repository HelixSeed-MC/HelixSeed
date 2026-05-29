@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%" || goto fail

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if "%VSROOT%"=="" goto no_vsroot

set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%"
if errorlevel 1 goto fail

if not exist "build" mkdir build
if not exist "lib" mkdir lib

cmake -S src -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=TRUE -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 goto fail

cmake --build build --config Release
if errorlevel 1 goto fail

if exist "build\cubiomes.dll" copy /Y "build\cubiomes.dll" "lib\lib.dll" >nul
if not exist "lib\lib.dll" goto fail

echo [cubiomes_26.1.2_fork] Built cubiomes_26.1.2_fork\lib\lib.dll
exit /b 0

:no_vswhere
echo [cubiomes_26.1.2_fork] vswhere.exe not found. Install Visual Studio Build Tools 2022.
exit /b 1

:no_vsroot
echo [cubiomes_26.1.2_fork] Visual C++ build tools not found.
exit /b 1

:no_vcvars
echo [cubiomes_26.1.2_fork] vcvars64.bat not found: %VCVARS%
exit /b 1

:fail
echo [cubiomes_26.1.2_fork] Build failed.
exit /b 1
