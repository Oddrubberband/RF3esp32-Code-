@echo off
setlocal
cd /d "%~dp0"
start "" "dist\RF3Desktop\RF3Desktop.exe" --backend serial
