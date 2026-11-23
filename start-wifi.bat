@echo off
setlocal
cd /d "%~dp0"
set "RADARHEX_WEB_PORT=8001"

where py >nul 2>nul
if errorlevel 1 (
  echo Python was not found. Install Python 3 and enable Add Python to PATH.
  pause
  exit /b 1
)

py -c "import fastapi, uvicorn, serial" >nul 2>nul
if errorlevel 1 (
  echo Running first-time RADARHEX setup...
  call setup.bat
)

rem Stop only the previous RADARHEX server on its dedicated port.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$listeners=Get-NetTCPConnection -LocalPort %RADARHEX_WEB_PORT% -State Listen -ErrorAction SilentlyContinue; if($listeners){$listeners | Select-Object -ExpandProperty OwningProcess -Unique | ForEach-Object {Stop-Process -Id $_ -Force}}" >nul 2>nul
timeout /t 1 /nobreak >nul

echo Starting RADARHEX Wi-Fi receiver on port %RADARHEX_WEB_PORT%...
start "RADARHEX Wi-Fi Server" /min cmd /c "py backend.py --mode wifi --web-port %RADARHEX_WEB_PORT% --no-browser"
timeout /t 3 /nobreak >nul
start "" "http://127.0.0.1:%RADARHEX_WEB_PORT%"
endlocal
