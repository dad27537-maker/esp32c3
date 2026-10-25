@echo off
cd /d "%~dp0.."
if exist ".venv\Scripts\python.exe" (
  ".venv\Scripts\python.exe" pc\receiver.py %*
) else (
  python pc\receiver.py %*
)
pause
