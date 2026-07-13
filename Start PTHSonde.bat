@echo off
REM ============================================================
REM  PTHSonde launcher  --  ALWAYS runs the latest SOURCE code.
REM  Kills any already-running instance first so you can never
REM  end up serving stale code or double-binding the port.
REM  (The built .exe in processor\dist is a stale snapshot --
REM   this .bat deliberately runs from source instead.)
REM ============================================================
setlocal
cd /d "%~dp0processor"

echo [PTHSonde] Stopping any running instance...
REM kill a stale packaged build
taskkill /F /IM PTHSonde.exe >nul 2>&1
REM kill any python/py process running app.py (old source instances)
wmic process where "commandline like '%%app.py%%' and (name='python.exe' or name='pythonw.exe' or name='py.exe')" call terminate >nul 2>&1

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

echo [PTHSonde] Launching latest source (app.py)...
%PY% app.py
endlocal
