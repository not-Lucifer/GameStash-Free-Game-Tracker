@echo off
cd build
FreeGameTracker.exe > ..\run_out.txt 2>&1
echo EXIT CODE: %ERRORLEVEL% >> ..\run_out.txt
