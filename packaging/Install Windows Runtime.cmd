@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0bundle\prerequisites\Install-Windows-Runtime.ps1"
if errorlevel 1 (
  echo.
  echo Runtime setup did not finish. The message above has Microsoft's download link.
)
echo.
pause
