@echo off
setlocal
set PYTHONUTF8=1
if "%~1"=="" (
  echo Usage: capture_tft.cmd COMx [output.png]
  echo Example: capture_tft.cmd COM13
  exit /b 2
)
"%USERPROFILE%\.platformio\penv\Scripts\python.exe" "%~dp0tools\capture_tft.py" %*
exit /b %errorlevel%
