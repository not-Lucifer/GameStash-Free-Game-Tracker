@echo off
setlocal enableextensions

set "ROOT=%~dp0"
rem Version comes from version.h (the trailing space skips GS_VERSION_STRING4)
for /f "tokens=3" %%V in ('findstr /c:"#define GS_VERSION_STRING " "%ROOT%version.h"') do set "VERSION=%%~V"
if not defined VERSION (echo Could not read version from version.h & exit /b 1)
set "DIST_NAME=GameStash-%VERSION%-win64"
set "DIST_DIR=%ROOT%dist\%DIST_NAME%"

call "%ROOT%build.bat"
if errorlevel 1 goto :error

if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

copy /y "%ROOT%build\GameStash.exe" "%DIST_DIR%" >nul
copy /y "%ROOT%index.html" "%DIST_DIR%" >nul
copy /y "%ROOT%style.css" "%DIST_DIR%" >nul
if exist "%ROOT%oauth_config.json.example" copy /y "%ROOT%oauth_config.json.example" "%DIST_DIR%" >nul
if exist "%ROOT%README.md" copy /y "%ROOT%README.md" "%DIST_DIR%" >nul

set "VCPKG_BIN=%ROOT%vcpkg\installed\x64-windows\bin"
for %%F in (brotlicommon.dll brotlidec.dll brotlienc.dll sqlite3.dll WebView2Loader.dll Microsoft.Web.WebView2.Core.dll) do (
    if exist "%VCPKG_BIN%\%%F" copy /y "%VCPKG_BIN%\%%F" "%DIST_DIR%" >nul
)

powershell -NoProfile -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%ROOT%dist\%DIST_NAME%.zip' -Force" >nul

echo Dist created: %DIST_DIR%
echo Zip: %ROOT%dist\%DIST_NAME%.zip

rem --- Installer --------------------------------------------------------------
rem Compiled here, straight from the dist folder staged above, so the installer
rem can never package stale files (compiling it by hand before re-running this
rem script shipped an old index.html). Any setup left from an earlier run is
rem deleted first, so a failed compile cannot leave an old one looking current.
set "SETUP=%ROOT%dist\GameStash-%VERSION%-Setup.exe"
if exist "%SETUP%" del /q "%SETUP%"

rem ProgramFiles(x86) contains parentheses, which break a parenthesised block,
rem so it is copied to a plain variable first.
set "PF86=%ProgramFiles(x86)%"
set "ISCC="
for %%I in ("%ProgramFiles%\Inno Setup 7\ISCC.exe" "%PF86%\Inno Setup 7\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%PF86%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do (
    if not defined ISCC if exist %%I set "ISCC=%%~I"
)
if not defined ISCC for %%I in (ISCC.exe) do set "ISCC=%%~$PATH:I"
if not defined ISCC (
    echo Inno Setup not found - installer skipped. Get it from https://jrsoftware.org/isinfo.php
    exit /b 0
)

"%ISCC%" "/DMyAppVersion=%VERSION%" "%ROOT%installer.iss"
if errorlevel 1 goto :setup_error
if not exist "%SETUP%" goto :setup_error
echo Installer: %SETUP%
exit /b 0

:error
echo Build failed. Dist not created.
exit /b 1

:setup_error
echo Installer compile failed. The dist folder and zip above are still valid.
exit /b 1
