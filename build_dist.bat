@echo off
setlocal enableextensions

set "ROOT=%~dp0"
set "VERSION=1.0.1"
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
exit /b 0

:error
echo Build failed. Dist not created.
exit /b 1
