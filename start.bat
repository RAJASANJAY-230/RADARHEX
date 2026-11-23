@echo off
setlocal
cd /d "%~dp0"

rem Change only these two values if the ESP32 port or web port changes.
set "RADARHEX_COM=COM14"
set "RADARHEX_WEB_PORT=8001"

rem If RADARHEX is already running, simply open the existing dashboard.
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $r=Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:%RADARHEX_WEB_PORT%/api/status' -TimeoutSec 2; if($r.StatusCode -eq 200){exit 0} } catch {}; exit 1" >nul 2>nul
if not errorlevel 1 (
  start "" "http://127.0.0.1:%RADARHEX_WEB_PORT%"
  exit /b 0
)

rem Verify that Python and the one-time dependencies are available.
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

echo Starting RADARHEX using %RADARHEX_COM% on port %RADARHEX_WEB_PORT%...
echo Keep Arduino Serial Monitor closed while using RADARHEX.
start "RADARHEX Server" /min cmd /c "py backend.py --port %RADARHEX_COM% --web-port %RADARHEX_WEB_PORT% --no-browser"
timeout /t 3 /nobreak >nul
start "" "http://127.0.0.1:%RADARHEX_WEB_PORT%"
endlocal
