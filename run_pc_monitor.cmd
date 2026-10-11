@echo off
setlocal
set "PYTHON=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=python"
"%PYTHON%" -c "import psutil" >nul 2>nul
if errorlevel 1 (
  echo 正在安装电脑网络统计依赖...
  "%PYTHON%" -m pip install -r "%~dp0requirements.txt"
  if errorlevel 1 (
    pause
    exit /b 2
  )
)
set "ESP32_TARGET="
if exist "%~dp0dashboard_ip.txt" (
  for /f "usebackq delims=" %%A in ("%~dp0dashboard_ip.txt") do if not defined ESP32_TARGET set "ESP32_TARGET=%%A"
)
if defined ESP32_TARGET (
  echo 正在向仪表地址 %ESP32_TARGET% 发送数据...
  "%PYTHON%" "%~dp0tools\pc_monitor.py" --broadcast "%ESP32_TARGET%"
) else (
  echo 未设置仪表地址，将尝试局域网广播。
  "%PYTHON%" "%~dp0tools\pc_monitor.py"
)
pause
