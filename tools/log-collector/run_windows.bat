@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel% == 0 (
    py -3 "%~dp0get_sysdvr_log.py"
    goto finish
)
where python >nul 2>nul
if %errorlevel% == 0 (
    python "%~dp0get_sysdvr_log.py"
    goto finish
)
echo Python 3 not found. Install Python 3.9+ from https://www.python.org/downloads/
echo Enable "Add Python to PATH" during installation.
:finish
echo.
pause
