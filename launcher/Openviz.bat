@echo off
rem Opens Openviz in your browser. Double-click this file, or make a shortcut to it.
rem It runs everything from this project folder, so it is always the current version.
cd /d "%~dp0.."
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 launcher\launcher.py %*
) else (
  python launcher\launcher.py %*
)
if errorlevel 1 pause
