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

REM Copy installer script
echo [*] Copying installer script...
copy "install.bat" "%DIST_FOLDER%\install.bat" >nul

REM Copy setup guide
echo [*] Copying setup guide...
copy "SETUP.md" "%DIST_FOLDER%\SETUP.md" >nul

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

REM Copy Brotli runtime if available
if exist "vcpkg\installed\x64-windows\bin\brotli.dll" (
    copy "vcpkg\installed\x64-windows\bin\brotli.dll" "%DIST_FOLDER%\brotli.dll" >nul
    echo     [+] brotli.dll copied
)

REM Copy all other necessary DLLs from vcpkg bin folder
echo     [+] Copying additional dependencies...
for %%F in (vcpkg\installed\x64-windows\bin\*.dll) do (
    if not exist "%DIST_FOLDER%\%%~nxF" (
        copy "%%F" "%DIST_FOLDER%\%%~nxF" >nul 2>&1
    )
)

REM Create README file using PowerShell to avoid batch syntax issues
echo [*] Creating README.md...
powershell -Command "Add-Content -Path '%DIST_FOLDER%\README.md' -Value '# Game Stash v%VERSION%'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value ''; Add-Content -Path '%DIST_FOLDER%\README.md' -Value 'A free, open-source game giveaway tracker for Windows'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value ''; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '## Installation'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value ''; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '### Option 1: Quick Install (Recommended)'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '1. Run install.bat as Administrator'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '2. Game Stash installed to Program Files with Start Menu shortcuts'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value ''; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '### Option 2: Portable Mode'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '1. Just double-click GameStash.exe'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value '2. No installation needed'; Add-Content -Path '%DIST_FOLDER%\README.md' -Value ''; Add-Content -Path '%DIST_FOLDER%\README.md' -Value 'See SETUP.md for detailed instructions.'" > nul 2>&1

if errorlevel 1 (
    echo [!] WARNING: Failed to create detailed README
    echo # Game Stash v%VERSION% > "%DIST_FOLDER%\README.md"
    echo. >> "%DIST_FOLDER%\README.md"
    echo See SETUP.md for installation instructions >> "%DIST_FOLDER%\README.md"
)

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

if errorlevel 1 (
    echo [!] WARNING: ZIP creation may have failed
    echo Attempting alternative method...
    
    if exist "%ProgramFiles%\7-Zip\7z.exe" (
        "%ProgramFiles%\7-Zip\7z.exe" a "%DIST_FOLDER%.zip" "%DIST_FOLDER%" >nul 2>&1
    )
)

echo.
echo ========================================
echo  Build Complete!
echo ========================================
echo.
echo [+] Distribution folder: %DIST_FOLDER%
echo [+] Portable ZIP: %DIST_FOLDER%.zip
echo.
echo Files included:
echo   - GameStash.exe ^(main application^)
echo   - index.html, style.css ^(web UI^)
echo   - WebView2Loader.dll ^(WebView2 runtime^)
echo   - sqlite3.dll ^(database engine^)
echo   - install.bat ^(Windows installer^)
echo   - SETUP.md ^(setup instructions^)
echo   - oauth_config.json.example ^(OAuth template^)
echo   - README.md ^(quick reference^)
echo.
echo Ready to distribute!
echo ========================================
echo.

pause
