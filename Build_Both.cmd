@echo off
setlocal
"%USERPROFILE%\.platformio\penv\Scripts\python.exe" "%~dp0build_boards.py" --build %*
exit /b %errorlevel%
