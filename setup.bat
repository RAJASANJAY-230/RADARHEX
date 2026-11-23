@echo off
setlocal
cd /d "%~dp0"
echo Installing RADARHEX local bridge requirements...
py -m pip install -r requirements.txt
if errorlevel 1 (
  echo.
  echo Installation failed. Confirm that Python 3 is installed and available as "py".
  pause
  exit /b 1
)
echo.
echo Setup complete. You can now run start.bat.
pause
