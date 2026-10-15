@echo off
rem Sets Openviz up on this PC: builds the core and the visualizer, and puts Openviz in the Start menu.
rem Double-click it. Run it again after an update, or if a step was not done: it picks up where it stopped.
cd /d "%~dp0.."
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 -X utf8 launcher\windows_setup.py %*
) else (
  python -X utf8 launcher\windows_setup.py %*
)
pause
