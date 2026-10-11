@echo off
setlocal
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
if "%~1"=="" (
  echo Usage: flash.cmd COMx
  echo Example: flash.cmd COM7
  exit /b 2
)
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e esp32d0wd -t upload --upload-port %1
exit /b %errorlevel%
