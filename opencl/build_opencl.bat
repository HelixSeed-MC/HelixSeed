@echo off
setlocal
set "SRC_DIR=%~dp0"
set "OUT_DLL=%SRC_DIR%gpu_filter_opencl.dll"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

where cl >nul 2>&1
if errorlevel 1 (
  if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" call "%%I\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)

where cl >nul 2>&1
if errorlevel 1 (
  echo [ERROR] cl.exe not found in PATH.
  echo Install Visual Studio Build Tools 2022 with C++ tools, then rerun.
  exit /b 1
)

pushd "%SRC_DIR%"
cl /nologo /std:c++17 /O2 /EHsc /LD /DGPU_FILTER_BUILD_DLL ^
   gpu_filter_opencl.cpp ^
   /Fe:gpu_filter_opencl.dll ^
   /link /OPT:REF /OPT:ICF
if errorlevel 1 (
  popd
  echo [ERROR] Build failed.
  exit /b 1
)

copy /Y "%OUT_DLL%" "..\\gpu_filter_opencl.dll" >nul
popd

echo [OK] Built: %OUT_DLL%
echo [OK] Copied to project root: ..\\gpu_filter_opencl.dll
exit /b 0
