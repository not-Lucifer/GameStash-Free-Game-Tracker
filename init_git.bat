@echo off
git init > git_out.log 2>&1
git add . >> git_out.log 2>&1
git commit -m "Initial commit" >> git_out.log 2>&1
echo EXIT CODE: %ERRORLEVEL% >> git_out.log
