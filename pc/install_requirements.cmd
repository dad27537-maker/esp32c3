@echo off
cd /d "%~dp0.."
if not exist ".venv\Scripts\python.exe" python -m venv .venv
if errorlevel 1 goto done
".venv\Scripts\python.exe" -m pip install -r pc\requirements.txt
:done
pause
