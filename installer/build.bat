@echo off
setlocal

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
    if defined HELIXSEED_VSROOT (
        if exist "%HELIXSEED_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" call "%HELIXSEED_VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
    )
)

where cl >nul 2>&1
if errorlevel 1 (
    echo [err] cl.exe not found. Install Visual Studio Build Tools 2022 with C++ tools, or set HELIXSEED_VSROOT.
    exit /b 1
)

pushd "%~dp0"

echo [*] Compiling HelixSeedInstaller.exe ...
rc /nologo /fo HelixSeedInstaller.res HelixSeedInstaller.rc
if errorlevel 1 (
    echo [err] Resource build failed.
    popd
    exit /b 1
)

cl /nologo /EHsc /W3 /std:c++17 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE HelixSeedInstaller.cpp HelixSeedInstaller.res ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib advapi32.lib ^
   /MANIFEST:EMBED /MANIFESTINPUT:HelixSeedInstaller.exe.manifest ^
   /OUT:HelixSeedInstaller.exe
if errorlevel 1 (
    echo [err] Build failed.
    popd
    exit /b 1
)

del /q HelixSeedInstaller.obj HelixSeedInstaller.res 2>nul

if defined HELIXSEED_SIGN_PFX (
    where signtool >nul 2>&1
    if errorlevel 1 (
        echo [warn] HELIXSEED_SIGN_PFX is set, but signtool.exe was not found. Installer left unsigned.
    ) else (
        echo [*] Signing HelixSeedInstaller.exe with PFX certificate ...
        if defined HELIXSEED_SIGN_PASSWORD (
            signtool sign /f "%HELIXSEED_SIGN_PFX%" /p "%HELIXSEED_SIGN_PASSWORD%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 HelixSeedInstaller.exe
        ) else (
            signtool sign /f "%HELIXSEED_SIGN_PFX%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 HelixSeedInstaller.exe
        )
        if errorlevel 1 (
            echo [err] Signing failed.
            popd
            exit /b 1
        )
    )
) else if defined HELIXSEED_SIGN_SHA1 (
    where signtool >nul 2>&1
    if errorlevel 1 (
        echo [warn] HELIXSEED_SIGN_SHA1 is set, but signtool.exe was not found. Installer left unsigned.
    ) else (
        echo [*] Signing HelixSeedInstaller.exe with certificate store thumbprint ...
        signtool sign /sha1 "%HELIXSEED_SIGN_SHA1%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 HelixSeedInstaller.exe
        if errorlevel 1 (
            echo [err] Signing failed.
            popd
            exit /b 1
        )
    )
)

echo.
echo [ok] Built %~dp0HelixSeedInstaller.exe
popd
endlocal
