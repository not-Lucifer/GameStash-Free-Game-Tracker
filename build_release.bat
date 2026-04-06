@echo off
REM Game Stash Release Build Script
REM Builds the app in Release mode and packages it for distribution

setlocal enabledelayedexpansion

echo.
echo ========================================
echo  Game Stash - Release Build Script
echo ========================================
echo.

REM Set version
set VERSION=1.0.0
set DIST_FOLDER=dist\GameStash_v%VERSION%

REM Clean old distribution folder
if exist "%DIST_FOLDER%" (
    echo [*] Removing old distribution folder...
    rmdir /s /q "%DIST_FOLDER%"
)

REM Create distribution folder structure
echo [*] Creating distribution folder structure...
mkdir "%DIST_FOLDER%"

REM Build the project in Release mode
echo [*] Building GameStash in Release mode...
echo     This may take a few seconds...
call build.bat >nul 2>&1

if not exist "build\GameStash.exe" (
    echo [!] ERROR: Build failed! GameStash.exe not found.
    exit /b 1
)

echo [+] Build successful!

REM Copy executable
echo [*] Copying GameStash.exe...
copy "build\GameStash.exe" "%DIST_FOLDER%\GameStash.exe" >nul
if errorlevel 1 (
    echo [!] ERROR: Failed to copy GameStash.exe
    exit /b 1
)

REM Copy web assets
echo [*] Copying web assets...
copy "index.html" "%DIST_FOLDER%\index.html" >nul
copy "style.css" "%DIST_FOLDER%\style.css" >nul

REM Copy configuration template
echo [*] Copying configuration files...
copy "oauth_config.json.example" "%DIST_FOLDER%\oauth_config.json.example" >nul

REM Copy WebView2 runtime loader if available
echo [*] Checking for WebView2 runtime files...
if exist "vcpkg\installed\x64-windows\bin\WebView2Loader.dll" (
    copy "vcpkg\installed\x64-windows\bin\WebView2Loader.dll" "%DIST_FOLDER%\WebView2Loader.dll" >nul
    echo     [+] WebView2Loader.dll copied
)

REM Copy SQLite3 runtime if available
if exist "vcpkg\installed\x64-windows\bin\sqlite3.dll" (
    copy "vcpkg\installed\x64-windows\bin\sqlite3.dll" "%DIST_FOLDER%\sqlite3.dll" >nul
    echo     [+] sqlite3.dll copied
)

REM Create README file
echo [*] Creating README...
(
    echo # Game Stash v%VERSION%
    echo.
    echo A free, open-source game giveaway tracker for Steam, Epic Games, GOG, and Itch.io
    echo.
    echo ## How to Use
    echo.
    echo 1. Extract this folder to your desired location
    echo 2. Double-click GameStash.exe to launch
    echo 3. Sign in or create an account to start tracking free games
    echo.
    echo ## Features
    echo.
    echo - Track free games from multiple platforms
    echo - Personal profile with customization
    echo - Email validation and secure authentication
    echo - Database-backed game library
    echo - Completely offline-capable
    echo.
    echo ## Configuration
    echo.
    echo To enable OAuth (optional):
    echo 1. Rename oauth_config.json.example to oauth_config.json
    echo 2. Add your OAuth credentials from Google/Discord
    echo 3. Restart the app
    echo.
    echo ## System Requirements
    echo.
    echo - Windows 10 or later
    echo - WebView2 runtime (auto-installed by the app if needed^)
    echo.
    echo ## License
    echo.
    echo Free to use and distribute
    echo.
) > "%DIST_FOLDER%\README.md"

REM Create a portable launcher script
echo [*] Creating launcher script...
(
    echo @echo off
    echo REM Game Stash Launcher
    echo cd /d "%%~dp0"
    echo GameStash.exe
) > "%DIST_FOLDER%\Launch.bat"

REM Create ZIP archive
echo [*] Creating portable ZIP archive...
if exist "%DIST_FOLDER%.zip" (
    del "%DIST_FOLDER%.zip"
)

powershell -Command "Compress-Archive -Path '%DIST_FOLDER%' -DestinationPath '%DIST_FOLDER%.zip' -Force" >nul 2>&1

echo.
echo ========================================
echo  Build Complete!
echo ========================================
echo.
echo [+] Distribution folder: %DIST_FOLDER%
echo [+] Portable ZIP: %DIST_FOLDER%.zip
echo.
echo Files included:
echo   - GameStash.exe (main application^)
echo   - index.html, style.css (web UI^)
echo   - WebView2Loader.dll (WebView2 runtime^)
echo   - sqlite3.dll (database engine^)
echo   - oauth_config.json.example (OAuth template^)
echo   - README.md (documentation^)
echo.
echo Ready to distribute!
echo ========================================
echo.

pause
