@echo off
setlocal
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e esp32d0wd %*
exit /b %errorlevel%
