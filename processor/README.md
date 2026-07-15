# PTHSonde processor / app backend

This is the Python side of the desktop app. `app.py` launches the Flask server
(`sonde_server.py`) and opens it in a native **pywebview** window. The server owns
the ground-receiver **COM port** (pyserial), records the flight CSV, serves
`Dashboard/PTHSonde.html`, runs **SHARPpy**, and renders the **SHARPpy SPC panel**
(the same SPC-style analysis used by Pivotal Weather and the Storm Prediction Center)
via `sharppy_render.py`.

> Run the app with `Start PTHSonde.bat` (repo root) or `py -3.13 app.py`.
> The two-Python-env note below is only about the **SHARPpy image render**.

## Architecture (two Python environments)
SHARPpy's GUI is built on Qt5, which does not run on modern Python versions, so the app uses two environments:

- **Main server — your Python 3.13** (`sonde_server.py`): the Flask API + SHARPpy
  thermodynamic/kinematic *index* calculations (these work on 3.13).
- **Render venv — Python 3.10** (`venv310/`): SHARPpy's actual GUI (git master) +
  **PySide2** + numpy 1.23, run **headless** (`QT_QPA_PLATFORM=offscreen`) by
  `sharppy_render.py` to produce the real SPC image. The 3.13 server shells out to it.

This split is required: SHARPpy's GUI needs Qt5/PySide2 (Python ≤3.10); PySide6
(Qt6) breaks on its unscoped enums and numpy-float draw calls.

## Setup (already done on this machine; here for reproducibility)
```
# main env (3.13)
pip install -r requirements.txt

# render env (3.10) — installs Python 3.10 first if needed
winget install -e --id Python.Python.3.10
py -3.10 -m venv venv310
venv310\Scripts\python -m pip install numpy==1.23.5 PySide2 qtpy requests python-dateutil
venv310\Scripts\python -m pip install "git+https://github.com/sharppy/SHARPpy.git" --no-deps
```

## Run
```
py -3.13 app.py            # or: Start PTHSonde.bat  (from the repo root)
```
This opens the native app window (Python owns the serial port; no browser, no
separate console). Flight folders default to `Documents\PTHSonde\`.

To build the standalone exe: `py -3.13 -m PyInstaller PTHSonde.spec`
→ `dist/PTHSonde/PTHSonde.exe`.

## Flow
New Flight -> creates `flight_YYYYMMDD_HHMMSSZ/` (UTC). Telemetry streams into its
`data.csv`. Process Flight -> reads the CSV, computes indices, renders the real
SHARPpy panel to `sharppy.png`, prepends the indices to the top of `data.csv`, and
the dashboard shows the panel on its Analysis tab.

If the 3.10 render env is missing/broken, the server falls back to a matplotlib
SPC-style panel (real indices, lookalike image) and notes it.
