@echo off
REM ============================================================
REM  PTHSonde launcher (dev / no-build mode)
REM  Runs the desktop app: one native window, serial owned by
REM  Python, no browser, no separate console. Double-click this.
REM
REM  Once you build the .exe (processor\dist\PTHSonde\PTHSonde.exe),
REM  just run that instead — this .bat is the source-run fallback.
REM ============================================================
setlocal
cd /d "%~dp0processor"

set "PY="
py -3.13 -c "import flask, webview, serial" >nul 2>&1 && set "PY=py -3.13"
if not defined PY ( python -c "import flask, webview, serial" >nul 2>&1 && set "PY=python" )
if not defined PY (
  echo [PTHSonde] Missing dependencies. Install them with:
  echo            py -3.13 -m pip install flask pyserial pywebview
  echo.
  pause
  exit /b 1
)

echo [PTHSonde] Launching app...
%PY% app.py
endlocal
