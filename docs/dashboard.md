# Ground-station app

A single-window Windows app that owns the serial link, records the flight, and shows
live atmospheric data + a real SHARPpy sounding. It is a **Flask server + a
self-contained HTML dashboard**, wrapped in a native window with **pywebview**.

## Architecture

```
processor/app.py            → launches the Flask server, then opens a native
                              pywebview window pointed at it
processor/sonde_server.py   → owns the COM port (pyserial, background reader +
                              auto-reconnect), records the flight CSV, serves the
                              dashboard, runs the SHARPpy analysis pipeline
Dashboard/PTHSonde.html     → the entire UI (canvas plots, no build step)
processor/sharppy_render.py → renders the genuine SHARPpy SPC panel, headless
```

**Why Python owns the serial port:** the browser's Web Serial API stalls silently
("connected but no data"). Moving the port into Python (with a ring buffer +
auto-reconnect) fixed the recurring drop-out; the dashboard just polls
`/serial/poll` over HTTP.

## Running it

- **Prebuilt:** download the app from [Releases](../../releases), unzip, run
  `PTHSonde.exe`.
- **From source:** `pip install -r processor/requirements.txt`, then
  `Start PTHSonde.bat` (or `py -3.13 processor/app.py`). The `.bat` runs live from
  source, so edits to the HTML/Python show up on the next launch.

## Using it

1. **Connect** — pick the ground receiver's COM port and click Connect. Telemetry
   streams into the sidebar + header KPIs (altitude, climb, speed, RSSI, SNR).
2. **Pair & Record** — when a sonde is detected, the popup lets you set the save
   folder (default `Documents\PTHSonde`) and start recording to
   `flight_YYYYMMDD_HHMMSSZ/data.csv`.
3. **Skew-T + Plots** — live Skew-T on the left; the right panel toggles between
   **Sounding** (hodograph, wind, θ, RH profiles), **Time Series** (temp / ascent /
   wind / dewpoint vs time), **System Health** (battery, radio signal-vs-noise,
   packet health), and **Map** (GFS-fed landing prediction).
4. **Stop Flight** → unlocks the **Analysis** tab.
5. **Process Flight** — runs SHARPpy, renders `sharppy.png`, and prepends the
   computed indices to the flight CSV.

Light/dark theme, a Console tab (raw feed + receiver commands), and a Radio Setup
window (channel stage/lock) are under the ⚙ Settings.

## Data quality processing

Two things the pipeline does automatically so the sounding isn't garbage:

- **Ascent trim** — drops pad rows before launch (first sustained pressure drop).
- **Thermal-soak trim** — on a sun-baked pad the T/RH sensors read hot and bleed
  that heat into the first part of the climb (a non-physical superadiabatic "warm
  nose"). The processor fits the *real* lapse rate from the clean free atmosphere
  (800–3500 m above launch), extrapolates it down, and drops the bottom rows that
  sit >1.5 °C above that line. Implemented in both `sonde_server.py` (`_trim_soak`,
  for CSV processing) and the live dashboard (`soakStart()`).

Altitude, ascent rate, and the whole sounding are derived from **barometric
pressure**, so they keep working above the GPS ceiling.

## SHARPpy render environment

SHARPpy's GUI is old Qt5 code that needs **Python 3.10 + PySide2**, so it lives in a
separate `processor/venv310/` (not committed — 453 MB) that the 3.13 server shells
out to. If that env is missing, the server falls back to a matplotlib Skew-T
(indices still computed). Setup steps are in
[`../processor/README.md`](../processor/README.md).

Headless-render gotcha: SHARPpy asks for the `Helvetica` font, which doesn't exist
on Windows and was being substituted by a symbol font (scrambled labels).
`sharppy_render.py` remaps it to Arial.

## Building the `.exe`

```bash
cd processor
py -3.13 -m PyInstaller PTHSonde.spec        # → dist/PTHSonde/PTHSonde.exe (onedir)
```

The spec bundles `Dashboard/` and `sharppy_render.py` and is frozen-path aware.
SHARPpy's `venv310` is **not** bundled (too heavy); ship it alongside or let the app
fall back to matplotlib.
