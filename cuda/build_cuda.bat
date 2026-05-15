@echo off
setlocal

set "SRC_DIR=%~dp0"
set "OUT_DLL=%SRC_DIR%gpu_filter.dll"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined CUDA_PATH (
  set "PATH=%CUDA_PATH%\bin;%PATH%"
)

where nvcc >nul 2>&1
if errorlevel 1 (
  echo [ERROR] nvcc not found.
  echo Install CUDA Toolkit and reopen terminal.
  exit /b 1
)

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
if not defined CUDA_ARCHES (
  set "CUDA_ARCHES=-gencode arch=compute_75,code=sm_75 -gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 -gencode arch=compute_90,code=sm_90 -gencode arch=compute_120,code=sm_120 -gencode arch=compute_121,code=sm_121 -gencode arch=compute_121,code=compute_121"
)
nvcc ^
  -std=c++17 ^
  -O3 ^
  -Xptxas -O3 ^
  -lineinfo ^
  %CUDA_ARCHES% ^
  -cudart static ^
  -DGPU_FILTER_BUILD_DLL ^
  -Xcompiler "/MD /O2 /EHsc /LD" ^
  gpu_filter.cu gpu_filter_wrapper.cpp ^
  -o gpu_filter.dll

if errorlevel 1 (
  popd
  echo [ERROR] Build failed.
  exit /b 1
)

copy /Y "%OUT_DLL%" "..\\gpu_filter.dll" >nul
popd

echo [OK] Built: %OUT_DLL%
echo [OK] Copied to project root: ..\\gpu_filter.dll
exit /b 0
