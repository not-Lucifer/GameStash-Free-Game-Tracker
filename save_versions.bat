@echo off
echo ============================================================
echo   FreeGameTracker - Git Version Protection Script
echo ============================================================
echo.

cd /d "d:\Projects\FreeGameTracekr"

echo [1/6] Checking git status...
git status
echo.

echo [2/6] Staging all current files (GUI version)...
git add -A
echo.

echo [3/6] Committing GUI version to main branch...
git commit -m "feat(gui): webview GUI with GamerPower API, URL resolver, and HTML frontend"
echo.

echo [4/6] Creating cli-version branch from this commit...
git branch cli-version
echo Branch created.
echo.

echo [5/6] Switching to cli-version branch...
git checkout cli-version
echo.

echo [6/6] Committing CLI source file to cli-version branch...
git add main_cli.cpp
git commit -m "feat(cli): working CLI version with console table output and URL resolver - preserved from before GUI migration"
echo.

echo Switching back to main (GUI) branch...
git checkout main
echo.

echo ============================================================
echo   DONE! Here is your branch summary:
echo ============================================================
git branch -v
echo.
git log --oneline -5
echo.
pause
