@echo off
setlocal
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
if "%~1"=="" (
  echo Usage: monitor.cmd COMx
  echo Example: monitor.cmd COM7
  exit /b 2
)
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" device monitor -p %1 -b 115200
exit /b %errorlevel%
