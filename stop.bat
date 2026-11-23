@echo off
setlocal
set "RADARHEX_WEB_PORT=8001"

powershell -NoProfile -ExecutionPolicy Bypass -Command "$listeners=Get-NetTCPConnection -LocalPort %RADARHEX_WEB_PORT% -State Listen -ErrorAction SilentlyContinue; if($listeners){$listeners | Select-Object -ExpandProperty OwningProcess -Unique | ForEach-Object {Stop-Process -Id $_ -Force}; exit 0}; exit 1" >nul 2>nul

if errorlevel 1 (
  echo RADARHEX is not currently running on port %RADARHEX_WEB_PORT%.
) else (
  echo RADARHEX has been stopped.
)
timeout /t 2 /nobreak >nul
endlocal
