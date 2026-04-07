@echo off
REM Game Stash Installer Script
REM Creates Start Menu shortcuts and installs the app

setlocal enabledelayedexpansion

echo.
echo ========================================
echo  Game Stash - Installation Wizard
echo ========================================
echo.

REM Check if running as admin
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] This installer requires Administrator privileges.
    echo [*] Please right-click and select "Run as administrator"
    echo.
    pause
    exit /b 1
)

REM Define paths
set APP_VERSION=1.0.0
set INSTALL_PATH=%ProgramFiles%\GameStash
set START_MENU=%APPDATA%\Microsoft\Windows\Start Menu\Programs\GameStash

echo [*] Installation Settings:
echo     Install Path: %INSTALL_PATH%
echo     Start Menu: %START_MENU%
echo.

REM Create installation directory
if not exist "%INSTALL_PATH%" (
    echo [*] Creating installation directory...
    mkdir "%INSTALL_PATH%"
)

REM Copy files from current directory
echo [*] Copying application files...
if exist "GameStash.exe" (
    copy "GameStash.exe" "%INSTALL_PATH%\GameStash.exe" >nul
    echo     [+] GameStash.exe
) else (
    echo [!] ERROR: GameStash.exe not found in current directory!
    exit /b 1
)

if exist "index.html" (
    copy "index.html" "%INSTALL_PATH%\index.html" >nul
    echo     [+] index.html
)

if exist "style.css" (
    copy "style.css" "%INSTALL_PATH%\style.css" >nul
    echo     [+] style.css
)

if exist "sqlite3.dll" (
    copy "sqlite3.dll" "%INSTALL_PATH%\sqlite3.dll" >nul
    echo     [+] sqlite3.dll
)

if exist "WebView2Loader.dll" (
    copy "WebView2Loader.dll" "%INSTALL_PATH%\WebView2Loader.dll" >nul
    echo     [+] WebView2Loader.dll
)

if exist "oauth_config.json.example" (
    if not exist "%INSTALL_PATH%\oauth_config.json" (
        copy "oauth_config.json.example" "%INSTALL_PATH%\oauth_config.json.example" >nul
        echo     [+] oauth_config.json.example
    )
)

REM Create Start Menu shortcuts directory
if not exist "%START_MENU%" (
    echo [*] Creating Start Menu folder...
    mkdir "%START_MENU%"
)

REM Create Start Menu shortcut
echo [*] Creating Start Menu shortcut...
call :create_shortcut "%START_MENU%\Game Stash.lnk" "%INSTALL_PATH%\GameStash.exe" "%INSTALL_PATH%" "Track free games from Steam, Epic, GOG & Itch.io" "C:\Windows\System32\imageres.dll,3"

REM Create Desktop shortcut (optional)
set DESKTOP=%USERPROFILE%\Desktop
if exist "%DESKTOP%" (
    echo [*] Creating Desktop shortcut...
    call :create_shortcut "%DESKTOP%\Game Stash.lnk" "%INSTALL_PATH%\GameStash.exe" "%INSTALL_PATH%" "Track free games from Steam, Epic, GOG & Itch.io" "C:\Windows\System32\imageres.dll,3"
)

REM Create uninstall shortcut
echo [*] Creating uninstall option...
call :create_shortcut "%START_MENU%\Uninstall Game Stash.lnk" "%INSTALL_PATH%\uninstall.bat" "%INSTALL_PATH%" "Uninstall Game Stash" "C:\Windows\System32\shell32.dll,131"

REM Create uninstall batch file
echo [*] Creating uninstall script...
(
    echo @echo off
    echo REM Game Stash Uninstaller
    echo echo.
    echo echo ========================================
    echo echo  Game Stash - Uninstall Wizard
    echo echo ========================================
    echo echo.
    echo.
    echo set /p confirm="Are you sure you want to uninstall Game Stash? (Y/N): "
    echo if /i not "!confirm!"=="Y" (
    echo     echo Uninstall cancelled.
    echo     pause
    echo     exit /b 0
    echo ^)
    echo.
    echo echo [*] Removing Game Stash...
    echo.
    echo REM Close the app if running
    echo taskkill /IM GameStash.exe /F 2^>nul
    echo timeout /t 2 /nobreak
    echo.
    echo REM Remove from Start Menu
    echo rmdir /s /q "%%APPDATA%%\Microsoft\Windows\Start Menu\Programs\GameStash" 2^>nul
    echo.
    echo REM Remove from Program Files
    echo cd /d "%%ProgramFiles%%"
    echo rmdir /s /q GameStash 2^>nul
    echo.
    echo echo [+] Game Stash has been uninstalled successfully.
    echo echo.
    echo pause
) > "%INSTALL_PATH%\uninstall.bat"

REM Create README
echo [*] Creating README...
(
    echo # Game Stash v%APP_VERSION%
    echo.
    echo A free, open-source game giveaway tracker for Steam, Epic Games, GOG, and Itch.io
    echo.
    echo ## Installation Complete!
    echo.
    echo Game Stash has been installed to:
    echo %INSTALL_PATH%
    echo.
    echo You can launch it from:
    echo - Start Menu ^(search for "Game Stash"^)
    echo - Desktop shortcut
    echo - File Explorer: %INSTALL_PATH%\GameStash.exe
    echo.
    echo ## Features
    echo - Track free games from multiple platforms
    echo - Personal profile with customization
    echo - Email validation and secure authentication
    echo - Database-backed game library
    echo - Completely offline-capable
    echo.
    echo ## Uninstall
    echo To uninstall, go to Start Menu ^> Game Stash ^> Uninstall Game Stash
    echo Or: Settings ^> Apps ^> Game Stash ^> Uninstall
    echo.
) > "%INSTALL_PATH%\README.md"

echo.
echo ========================================
echo  Installation Complete!
echo ========================================
echo.
echo [+] Game Stash v%APP_VERSION% installed successfully!
echo.
echo [*] Launch from:
echo     - Start Menu (search "Game Stash"^)
echo     - Desktop shortcut (if created^)
echo     - %INSTALL_PATH%\GameStash.exe
echo.
echo [*] To uninstall:
echo     - Start Menu ^> Game Stash ^> Uninstall Game Stash
echo     - Or: Control Panel ^> Programs ^> Uninstall a program
echo.
echo ========================================
echo.

REM Launch the app
echo [*] Launching Game Stash...
start "" "%INSTALL_PATH%\GameStash.exe"

timeout /t 2 /nobreak

exit /b 0

REM ─── Function to create shortcuts ────────────────────────────────────────────
:create_shortcut
setlocal
set SHORTCUT_PATH=%~1
set TARGET=%~2
set WORK_DIR=%~3
set DESCRIPTION=%~4
set ICON=%~5

REM Create shortcut using PowerShell (more reliable than VBScript)
powershell -Command "^
    $WshShell = New-Object -ComObject WScript.Shell; ^
    $Shortcut = $WshShell.CreateShortcut('%SHORTCUT_PATH%'); ^
    $Shortcut.TargetPath = '%TARGET%'; ^
    $Shortcut.WorkingDirectory = '%WORK_DIR%'; ^
    $Shortcut.Description = '%DESCRIPTION%'; ^
    if ('%ICON%' -ne '') { $Shortcut.IconLocation = '%ICON%' }; ^
    $Shortcut.Save(); ^
" >nul 2>&1

endlocal
exit /b 0
